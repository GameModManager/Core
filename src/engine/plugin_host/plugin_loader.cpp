#include "engine/plugin_host/plugin_loader.h"
#include "engine/plugin_host/python_loader.h"
#include "engine/plugin_host/diagnostics_registry.h"
#include "engine/events/event_bus.h"
#include "engine/log/logger.h"
#include "engine/model/mod.h"
#include "engine/pipeline/fomod_stage.h"
#include "engine/pipeline/pipeline.h"
#include "engine/plugins/plugin_database.h"
#include "engine/registry/game_features/game_feature_registry.h"
#include "engine/sort/sort_registry.h"
#include "engine/sort/abi_sort_provider.h"

#include "gmm_abi_v1.h"

#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>

namespace engine {

// Bridging context passed as user_data to plugin registration callbacks
struct RegistrationBridge {
    PluginLoader* loader = nullptr;
    PluginInfo* current_plugin = nullptr;
};

namespace {

// PipelineContext of the stage claim currently executing on this thread. Set
// around the plugin handler invocation so the host UI bridge callbacks
// (GmmHostUi::fomod_wizard) can re-enter the engine on the same Mod + context
// the plugin was handed. Only valid inside the handler; the host marshals any
// UI onto the main thread itself, so the pointer stays valid for the whole
// call, and the bridge refuses to run without it (e.g. called on a worker
// thread spawned by the plugin).
thread_local PipelineContext* g_active_stage_ctx = nullptr;

// Minimal JSON string escaping for the host->plugin result payloads.
std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x",
                                  static_cast<unsigned int>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

}  // namespace

// ABI callback implementations
static void cb_register_identity(GmmRegistrationCtx* ctx,
                                  uint32_t steam_appid,
                                  const char* gog_id,
                                  const char* epic_namespace,
                                  const char* nexus_domain,
                                  const char* display_name,
                                  const char* exe_windows,
                                  const char* exe_linux,
                                  const char* exe_macos) {
    auto* bridge = static_cast<RegistrationBridge*>(ctx->user_data);
    if (!bridge || !bridge->current_plugin) return;

    bridge->current_plugin->steam_appid = steam_appid;
    bridge->current_plugin->nexus_domain = nexus_domain ? nexus_domain : "";
    if (display_name)
        bridge->current_plugin->game_display_name = display_name;
    // register_identity is THE game-support marker: only game plugins call it,
    // so game_plugins() / the create-instance list keys off this flag.
    bridge->current_plugin->game_support = true;

    Logger::instance().debug("Plugin registered identity: appid=" +
        std::to_string(steam_appid) +
        " name=" + (display_name ? display_name : bridge->current_plugin->game_id) +
        " nexus=" + (nexus_domain ? std::string(nexus_domain) : "none"));
}

static void cb_register_meta(GmmRegistrationCtx* ctx,
                             const char* author,
                             const char* version,
                             const char* description) {
    auto* bridge = static_cast<RegistrationBridge*>(ctx->user_data);
    if (!bridge || !bridge->current_plugin) return;

    if (author) bridge->current_plugin->author = author;
    if (version) bridge->current_plugin->version = version;
    if (description) bridge->current_plugin->description = description;
}

static void cb_register_category(GmmRegistrationCtx* ctx,
                                 const char* category) {
    auto* bridge = static_cast<RegistrationBridge*>(ctx->user_data);
    if (!bridge || !bridge->current_plugin) return;

    if (category) bridge->current_plugin->category = category;
}

static void cb_register_settings(GmmRegistrationCtx* ctx,
                                 const char* const* keys,
                                 const char* const* values,
                                 size_t count) {
    auto* bridge = static_cast<RegistrationBridge*>(ctx->user_data);
    if (!bridge || !bridge->current_plugin || !keys || !values) return;

    auto& settings = bridge->current_plugin->settings;
    for (size_t i = 0; i < count; ++i) {
        if (!keys[i] || !values[i]) continue;
        settings.emplace_back(keys[i], values[i]);
    }
}

static void cb_register_settings_tab(GmmRegistrationCtx* ctx,
                                     const char* title,
                                     const char* const* keys,
                                     const char* const* types,
                                     const char* const* defaults,
                                     const char* const* options,
                                     size_t count) {
    auto* bridge = static_cast<RegistrationBridge*>(ctx->user_data);
    if (!bridge || !bridge->current_plugin || !title || !keys || !types) return;

    PluginInfo::SettingTab tab;
    tab.title = title;
    for (size_t i = 0; i < count; ++i) {
        if (!keys[i] || !types[i]) continue;
        PluginInfo::SettingTabEntry entry;
        entry.key = keys[i];
        entry.type = types[i];
        if (defaults && defaults[i]) entry.default_value = defaults[i];
        if (options && options[i]) {
            if (entry.type == "choice") {
                // newline-separated candidate choices
                std::string opts = options[i];
                size_t pos = 0;
                while ((pos = opts.find('\n')) != std::string::npos) {
                    entry.choices.emplace_back(opts.substr(0, pos));
                    opts.erase(0, pos + 1);
                }
                if (!opts.empty()) entry.choices.emplace_back(std::move(opts));
            } else if (entry.type == "int") {
                entry.int_range = options[i];
            }
        }
        tab.settings.push_back(std::move(entry));
    }
    bridge->current_plugin->settings_tab = std::move(tab);
}

static void cb_register_diagnostics(GmmRegistrationCtx* ctx,
                                    GmmDiagnosticsFn fn,
                                    void* user_data) {
    auto* bridge = static_cast<RegistrationBridge*>(ctx->user_data);
    if (!bridge || !bridge->current_plugin) return;

    DiagnosticsRegistry::instance().register_provider(
        bridge->current_plugin->game_id, fn, user_data);
}

static void cb_register_game_feature(GmmRegistrationCtx* ctx,
                                     const char* game_id,
                                     const char* feature_type,
                                     int priority,
                                     const char* const* folder_names,
                                     size_t folder_count,
                                     const char* const* file_extensions,
                                     size_t extension_count) {
    auto* bridge = static_cast<RegistrationBridge*>(ctx->user_data);
    if (!bridge || !bridge->current_plugin) return;

    // NULL game_id = this plugin's own game (matches how every other
    // registration keys itself); an explicit game_id lets a standalone plugin
    // override a game it doesn't provide (the P1.2 override test does this).
    std::string gid = game_id ? game_id : bridge->current_plugin->game_id;
    std::string type = feature_type ? feature_type : "";
    if (gid.empty() || type.empty()) {
        Logger::instance().warn("Game feature registered with empty game_id/type");
        return;
    }

    if (type == "mod_data_checker") {
        std::vector<std::string> folders;
        if (folder_names) {
            for (size_t i = 0; i < folder_count; ++i)
                if (folder_names[i]) folders.emplace_back(folder_names[i]);
        }
        std::vector<std::string> extensions;
        if (file_extensions) {
            for (size_t i = 0; i < extension_count; ++i)
                if (file_extensions[i]) extensions.emplace_back(file_extensions[i]);
        }
        auto checker = std::make_shared<ModDataCheckerFeature>(
            std::move(folders), std::move(extensions));
        GameFeatureRegistry::instance().register_feature(
            gid, type, priority, std::move(checker), bridge->current_plugin->path);
    } else if (type == "game_plugins") {
        // The game's vanilla plugin files (MO2 GamePlugins::gamePlugins()):
        // Skyrim's ESMs + _ResourcePack.esl head the unmanaged top band. The
        // plugin names ride the folder_names array slot — the ABI's two string
        // arrays are generic payload slots interpreted per feature type.
        std::vector<std::string> plugins;
        if (folder_names) {
            for (size_t i = 0; i < folder_count; ++i)
                if (folder_names[i]) plugins.emplace_back(folder_names[i]);
        }
        auto feature = std::make_shared<GamePluginsFeature>(std::move(plugins));
        GameFeatureRegistry::instance().register_feature(
            gid, type, priority, std::move(feature), bridge->current_plugin->path);
    } else {
        Logger::instance().warn("Plugin registered unknown game feature type: " +
            type + " (ignored)");
        return;
    }

    Logger::instance().debug("Plugin registered game feature: " + type +
        " (game=" + gid + ", priority=" + std::to_string(priority) + ")");
}

static void cb_register_game_feature_data(GmmRegistrationCtx* ctx,
                                          const char* game_id,
                                          const char* feature_type,
                                          int priority,
                                          const char* const* keys,
                                          const char* const* values,
                                          size_t count) {
    auto* bridge = static_cast<RegistrationBridge*>(ctx->user_data);
    if (!bridge || !bridge->current_plugin) return;

    // NULL game_id = this plugin's own game (same rule as every other
    // registration); an explicit game_id lets a standalone plugin override a
    // game it doesn't provide.
    std::string gid = game_id ? game_id : bridge->current_plugin->game_id;
    std::string type = feature_type ? feature_type : "";
    if (gid.empty() || type.empty()) {
        Logger::instance().warn("Game feature data registered with empty game_id/type");
        return;
    }

    // The 7 structured-data feature types (mod_data_content, data_archives,
    // script_extender, save_game_info, local_savegames, unmanaged_mods,
    // bsa_invalidation) parse through the shared registry function, so the
    // key-value contract lives once and the pybind mirror stays identical.
    std::vector<std::pair<std::string, std::string>> kv;
    if (keys && values) {
        for (size_t i = 0; i < count; ++i)
            if (keys[i]) kv.emplace_back(keys[i], values[i] ? values[i] : "");
    }
    if (!engine::register_game_feature_data(gid, type, priority, std::move(kv),
                                            bridge->current_plugin->path)) {
        return;  // register_game_feature_data already logged the reason
    }

    Logger::instance().debug("Plugin registered game feature: " + type +
        " (game=" + gid + ", priority=" + std::to_string(priority) + ")");
}

static void cb_subscribe_event(GmmRegistrationCtx* ctx,
                               const char* event_id,
                               GmmEventFn fn,
                               void* user_data) {
    auto* bridge = static_cast<RegistrationBridge*>(ctx->user_data);
    if (!bridge || !bridge->current_plugin) return;
    if (!event_id || !fn) {
        Logger::instance().warn("subscribe_event called with null event_id/fn");
        return;
    }

    // The bus owns the std::function, so the captured GmmEventFn+user_data
    // live for the subscription's lifetime; unload_all() drops every
    // subscription registered under this plugin's path before dlclose, so the
    // pointer never outlives the .so that owns fn.
    engine::EventBus::instance().subscribe(
        event_id,
        [fn, user_data](const std::string& eid, const std::string& payload) {
            fn(eid.c_str(), payload.c_str(), user_data);
        },
        bridge->current_plugin->path);

    Logger::instance().debug("Plugin subscribed to event: " +
        std::string(event_id) + " (plugin=" + bridge->current_plugin->game_id + ")");
}

// P1.4 — GmmHostUi::fomod_wizard: the plugin's Fomod stage handler asks the
// host to run the FOMOD wizard + install for the mod it is processing. The
// engine's Qt-free FomodStage does all the work (detect fomod/, parse
// ModuleConfig.xml, apply the chosen options to the staging dir, flatten);
// the wizard itself is the host's fomod_query_cb (wired by the UI to the
// native dialog), invoked on the pipeline thread exactly as in the core
// FomodStage path. The outcome goes back to the plugin as JSON.
//
// No ctx is passed: the pipeline context of the plugin's running stage comes
// from the thread-local set around the handler invocation, so the plugin can
// call this from a handler it cached the function pointer of — never from a
// cached GmmRegistrationCtx (that is host storage, valid only for
// gmm_register_v1).
static int cb_fomod_wizard(GmmModHandle mod,
                           char* out_json,
                           size_t out_capacity) {
    auto* m = reinterpret_cast<Mod*>(mod);
    if (!m || !out_json || out_capacity == 0) return 0;
    out_json[0] = '\0';

    PipelineContext* pctx = g_active_stage_ctx;
    if (!pctx) {
        Logger::instance().warn("host_ui.fomod_wizard called outside a stage handler");
        return 0;
    }

    FomodStage stage;
    const bool ok = stage.execute(*m, *pctx);

    // Serialize the outcome. fomod_detected separates "not a FOMOD"
    // (pass-through) from a real FOMOD install; canceled vs failed via the
    // stage's own context flag (FomodStage sets ctx.canceled on wizard
    // cancel). choices carries the persisted fomod.json object verbatim.
    std::string json;
    if (!pctx->fomod_detected) {
        json = "{\"outcome\":\"not_fomod\"}";
    } else if (pctx->canceled) {
        json = "{\"outcome\":\"canceled\"}";
    } else if (!ok) {
        json = "{\"outcome\":\"failed\"}";
    } else {
        json = "{\"outcome\":\"installed\",\"final_name\":\"" + json_escape(m->name) +
               "\",\"choices\":" +
               (pctx->fomod_choices_json.empty() ? "null" : pctx->fomod_choices_json) +
               "}";
    }
    if (json.size() >= out_capacity) {
        Logger::instance().warn("host_ui.fomod_wizard: result does not fit the plugin buffer");
        return 0;
    }
    std::memcpy(out_json, json.c_str(), json.size() + 1);
    return 1;
}

static void cb_register_stage_claim(GmmRegistrationCtx* ctx,
                                     const char* stage_name,
                                     GmmStageFn fn,
                                     int priority) {
    auto* bridge = static_cast<RegistrationBridge*>(ctx->user_data);
    if (!bridge || !bridge->current_plugin) return;

    std::string game_id = bridge->current_plugin->game_id;
    std::string stage = stage_name ? stage_name : "";
    if (stage.empty() || !fn) return;

    bridge->loader->stage_registry().register_claim(
        game_id, stage,
        [fn](Mod& mod, PipelineContext& ctx_) -> bool {
            // Wrap the real engine objects in the opaque handles the ABI
            // promises.  Accessors in abi_bridge.cpp are null-safe, so a
            // context without instance/conflict/profile still works.
            GmmModHandle mod_h = reinterpret_cast<GmmModHandle>(&mod);
            GmmInstanceHandle inst_h =
                reinterpret_cast<GmmInstanceHandle>(ctx_.instance);
            GmmConflictIndexHandle conf_h =
                reinterpret_cast<GmmConflictIndexHandle>(ctx_.conflict_index);
            GmmProfileHandle prof_h =
                reinterpret_cast<GmmProfileHandle>(ctx_.profile);
            // The context is live only for the plugin handler's call: the
            // host UI bridge (ctx.host_ui.*) re-enters engine stages on it,
            // so the pointer can never outlive the invoke.
            g_active_stage_ctx = &ctx_;
            const int result = fn(mod_h, inst_h, conf_h, prof_h, nullptr);
            g_active_stage_ctx = nullptr;
            return result != 0;
        },
        priority, bridge->current_plugin->path);
}

static void cb_register_hook(GmmRegistrationCtx* ctx,
                              const char* tag,
                              const char* data,
                              GmmHookFn fn,
                              int priority,
                              void* user_data) {
    auto* bridge = static_cast<RegistrationBridge*>(ctx->user_data);
    if (!bridge || !bridge->current_plugin) return;

    std::string game_id = bridge->current_plugin->game_id;
    std::string hook_tag = tag ? tag : "";
    std::string hook_data = data ? data : "";

    // Store as game knowledge - key=tag, value=data
    bridge->loader->knowledge().set(game_id, hook_tag, hook_data);

    Logger::instance().debug("Plugin registered knowledge: " + hook_tag +
        " (game=" + game_id + ", data=" + hook_data + ")");
    (void)fn; (void)priority; (void)user_data;
}

static void cb_register_order_encoding(GmmRegistrationCtx* ctx,
                                        GmmOrderEncodingFn fn) {
    auto* bridge = static_cast<RegistrationBridge*>(ctx->user_data);
    if (!bridge) return;

    // TODO: Store order encoding callback in plugin info for pipeline use
    Logger::instance().debug("Plugin registered order encoding hook");
    (void)fn;
}

static void cb_register_deploy_strategy(GmmRegistrationCtx* ctx,
                                         GmmDeployFn deploy_fn,
                                         GmmRemoveFn remove_fn) {
    auto* bridge = static_cast<RegistrationBridge*>(ctx->user_data);
    if (!bridge) return;

    Logger::instance().debug("Plugin registered deploy strategy");
    (void)deploy_fn; (void)remove_fn;
}

static void cb_register_tool(GmmRegistrationCtx* ctx,
                              const char* tool_id,
                              const char* kind,
                              void (*invoke_fn)(void*),
                              void* user_data) {
    auto* bridge = static_cast<RegistrationBridge*>(ctx->user_data);
    if (!bridge || !bridge->current_plugin) return;

    ExternalTool tool;
    tool.tool_id = tool_id ? tool_id : "";
    tool.game_id = bridge->current_plugin->game_id;
    tool.display_name = tool.tool_id;

    std::string kind_str = kind ? kind : "advisory";
    tool.kind = (kind_str == "workshop") ? ToolKind::Workshop : ToolKind::Advisory;

    if (invoke_fn) {
        tool.invoke_fn = [invoke_fn](void* ud) { invoke_fn(ud); };
        tool.invoke_user_data = user_data;
    }

    bridge->loader->tool_registry().register_tool(tool);

    Logger::instance().debug("Plugin registered tool: " + tool.tool_id +
        " (" + kind_str + ") for game=" + tool.game_id);
}

static void cb_register_image_diff(GmmRegistrationCtx* ctx,
                                    GmmImageDiffFn fn,
                                    void* user_data) {
    auto* bridge = static_cast<RegistrationBridge*>(ctx->user_data);
    if (!bridge) return;

    bridge->loader->register_image_diff(fn, user_data);

    Logger::instance().debug("Plugin registered image diff provider");
}

static void cb_register_sort_provider(GmmRegistrationCtx* ctx,
                                       const char* game_id,
                                       SortFn sort_fn,
                                       void* user_data) {
    auto* bridge = static_cast<RegistrationBridge*>(ctx->user_data);
    if (!bridge) return;

    std::string gid = game_id ? game_id : "";
    if (gid.empty()) {
        Logger::instance().warn("Sort provider registered with empty game_id");
        return;
    }

    auto provider = std::make_unique<AbiSortProvider>(gid.c_str(), sort_fn, user_data);
    SortRegistry::instance().register_provider(gid, std::move(provider));

    Logger::instance().debug("Plugin registered sort provider for game=" + gid);
}

static void cb_register_capability(GmmRegistrationCtx* ctx,
                                    const char* capability,
                                    const char* display_name,
                                    const char* data_path,
                                    const char* description,
                                    const char* protocol_handler,
                                    const char* website_domain,
                                    const char* supported_platforms) {
    auto* bridge = static_cast<RegistrationBridge*>(ctx->user_data);
    if (!bridge || !bridge->current_plugin) return;

    CapabilityInfo info;
    info.game_id = bridge->current_plugin->game_id;
    info.capability = capability ? capability : "";
    info.display_name = display_name ? display_name : capability ? capability : "";
    info.data_path = data_path ? data_path : "";
    info.description = description ? description : "";
    info.protocol_handler = protocol_handler ? protocol_handler : "";
    info.website_domain = website_domain ? website_domain : "";

    // Parse comma-separated platforms
    if (supported_platforms) {
        std::string platforms_str = supported_platforms;
        size_t pos = 0;
        while ((pos = platforms_str.find(',')) != std::string::npos) {
            info.supported_platforms.push_back(platforms_str.substr(0, pos));
            platforms_str.erase(0, pos + 1);
        }
        if (!platforms_str.empty()) {
            info.supported_platforms.push_back(platforms_str);
        }
    }

    bridge->loader->capabilities().register_capability(info);
}

static void cb_register_tab(GmmRegistrationCtx* ctx,
                             const char* capability,
                             const char* display_name,
                             const char* data_path,
                             const char* description,
                             const char* protocol_handler,
                             const char* website_domain,
                             const char* supported_platforms,
                             const char* insert_before,
                             const char* insert_after) {
    auto* bridge = static_cast<RegistrationBridge*>(ctx->user_data);
    if (!bridge || !bridge->current_plugin) return;

    CapabilityInfo info;
    info.game_id = bridge->current_plugin->game_id;
    info.capability = capability ? capability : "";
    info.display_name = display_name ? display_name : capability ? capability : "";
    info.data_path = data_path ? data_path : "";
    info.description = description ? description : "";
    info.protocol_handler = protocol_handler ? protocol_handler : "";
    info.website_domain = website_domain ? website_domain : "";
    info.insert_before = insert_before ? insert_before : "";
    info.insert_after = insert_after ? insert_after : "";

    // Parse comma-separated platforms
    if (supported_platforms) {
        std::string platforms_str = supported_platforms;
        size_t pos = 0;
        while ((pos = platforms_str.find(',')) != std::string::npos) {
            info.supported_platforms.push_back(platforms_str.substr(0, pos));
            platforms_str.erase(0, pos + 1);
        }
        if (!platforms_str.empty()) {
            info.supported_platforms.push_back(platforms_str);
        }
    }

    bridge->loader->capabilities().register_capability(info);
}

PluginLoader::~PluginLoader() {
    unload_all();
}

bool PluginLoader::load_plugin(const std::string& path) {
    if (is_loaded(path)) {
        Logger::instance().warn("Plugin already loaded: " + path);
        return true;
    }

    if (is_disabled(std::filesystem::path(path).filename().string())) {
        Logger::instance().debug("Plugin disabled in settings, skipping: " + path);
        return false;
    }

    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        Logger::instance().error("Failed to load plugin: " + path + " - " + dlerror());
        return false;
    }

    // Check ABI version
    auto version_fn = reinterpret_cast<uint32_t (*)()>(dlsym(handle, "gmm_abi_version"));
    if (!version_fn) {
        Logger::instance().error("Plugin missing gmm_abi_version: " + path);
        dlclose(handle);
        return false;
    }

    uint32_t plugin_abi = version_fn();
    if (plugin_abi != GMM_ABI_VERSION) {
        Logger::instance().error("ABI version mismatch: plugin=" +
            std::to_string(plugin_abi) + " host=" + std::to_string(GMM_ABI_VERSION));
        dlclose(handle);
        return false;
    }

    // Get registration function
    auto register_fn = reinterpret_cast<void (*)(GmmRegistrationCtx*)>(
        dlsym(handle, "gmm_register_v1"));
    if (!register_fn) {
        Logger::instance().error("Plugin missing gmm_register_v1: " + path);
        dlclose(handle);
        return false;
    }

    PluginInfo info;
    info.path = path;
    info.game_id = std::filesystem::path(path).stem().string();
    info.game_display_name = info.game_id;  // fallback, overridden by register_identity
    info.abi_version = plugin_abi;
    info.loaded = true;
    info.handle = handle;

    // Set up registration context and call plugin
    GmmRegistrationCtx ctx = {};
    ctx.register_identity = cb_register_identity;
    ctx.register_stage_claim = cb_register_stage_claim;
    ctx.register_hook = cb_register_hook;
    ctx.register_order_encoding = cb_register_order_encoding;
    ctx.register_deploy_strategy = cb_register_deploy_strategy;
    ctx.register_tool = cb_register_tool;
    ctx.register_sort_provider = cb_register_sort_provider;
    ctx.register_image_diff = cb_register_image_diff;
    ctx.register_capability = cb_register_capability;
    ctx.register_tab = cb_register_tab;
    ctx.register_meta = cb_register_meta;
    ctx.register_category = cb_register_category;
    ctx.register_settings = cb_register_settings;
    ctx.register_settings_tab = cb_register_settings_tab;
    ctx.register_diagnostics = cb_register_diagnostics;
    ctx.register_game_feature = cb_register_game_feature;
    ctx.register_game_feature_data = cb_register_game_feature_data;
    ctx.subscribe_event = cb_subscribe_event;
    ctx.host_ui.fomod_wizard = cb_fomod_wizard;

    RegistrationBridge bridge;
    bridge.loader = this;
    bridge.current_plugin = &info;
    ctx.user_data = &bridge;

    register_fn(&ctx);

    info.registered = true;
    plugins_.push_back(info);

    Logger::instance().debug("Plugin registered: " + info.game_display_name +
        " (" + path + ", game=" + info.game_id +
        ", appid=" + std::to_string(info.steam_appid) + ")");
    return true;
}

bool PluginLoader::load_directory(const std::string& dir_path) {
    namespace fs = std::filesystem;

    if (!fs::exists(dir_path) || !fs::is_directory(dir_path)) {
        Logger::instance().warn("Plugin directory not found: " + dir_path);
        return false;
    }

    int loaded = 0;
    for (const auto& entry : fs::directory_iterator(dir_path)) {
        if (!entry.is_regular_file()) continue;

        const auto& path = entry.path();
        auto ext = path.extension().string();

        // Platform-appropriate shared library extensions
#ifdef __linux__
        if (ext == ".so") {
            if (load_plugin(path.string())) loaded++;
        }
#elif defined(_WIN32)
        if (ext == ".dll") {
            if (load_plugin(path.string())) loaded++;
        }
#elif defined(__APPLE__)
        if (ext == ".dylib") {
            if (load_plugin(path.string())) loaded++;
        }
#endif

        // Python plugins - always attempted regardless of OS
        if (ext == ".py") {
            if (is_disabled(path.filename().string())) {
                Logger::instance().debug("Plugin disabled in settings, skipping: " + path.string());
                continue;
            }
            if (python_load_plugin(this, path.string())) loaded++;
        }
    }

    Logger::instance().debug("Loaded " + std::to_string(loaded) + " plugins from " + dir_path);

    std::string list_str;
    for (size_t i = 0; i < plugins_.size(); ++i) {
        if (i > 0) list_str += ", ";
        list_str += plugins_[i].game_display_name;
    }
    Logger::instance().debug("Loaded plugins: [" + list_str + "]");
    return loaded > 0;
}

bool PluginLoader::is_loaded(const std::string& path) const {
    for (const auto& p : plugins_) {
        if (p.path == path) return true;
    }
    return false;
}

bool PluginLoader::is_disabled(const std::string& filename) const {
    for (const auto& name : disabled_plugins_) {
        if (name == filename) return true;
    }
    return false;
}

void PluginLoader::add_loaded_plugin(PluginInfo info) {
    plugins_.push_back(std::move(info));
}

void PluginLoader::collect_diagnostics(const std::string& game_id, PluginDatabase& db) {
    DiagnosticsRegistry::instance().collect(game_id, db);
}

void PluginLoader::unload_all() {
    for (auto& p : plugins_) {
        // Drop this plugin's event subscriptions BEFORE dlclose so no bus
        // callback can ever run against unloaded .so code.
        EventBus::instance().clear_source(p.path);
        if (p.handle) {
            dlclose(p.handle);
            p.handle = nullptr;
        }
    }
    plugins_.clear();
}

std::string PluginLoader::display_name_for(const std::string& game_id) const {
    for (const auto& p : plugins_) {
        if (p.game_id == game_id) return p.game_display_name;
    }
    // Fallback: resolve via fuzzy match
    auto resolved = resolve_game_id(game_id);
    if (resolved != game_id) return display_name_for(resolved);
    return game_id;
}

std::string PluginLoader::resolve_game_id(const std::string& game_id) const {
    // Exact match - fast path
    for (const auto& p : plugins_)
        if (p.game_id == game_id) return game_id;

    // Fuzzy: check if any plugin's game_id contains the query or vice versa
    // (handles shortname→fullname renames like "isaac" ↔ "TheBindingOfIsaacRebirth")
    std::string q_lower;
    for (char c : game_id) q_lower += static_cast<char>(std::tolower(c));

    for (const auto& p : plugins_) {
        std::string p_lower;
        for (char c : p.game_id) p_lower += static_cast<char>(std::tolower(c));

        if (p_lower.find(q_lower) != std::string::npos ||
            q_lower.find(p_lower) != std::string::npos)
            return p.game_id;
    }

    // Fuzzy: try normalizing display name to instance-name format
    // (spaces → underscores, remove illgal chars)
    for (const auto& p : plugins_) {
        std::string norm;
        for (char c : p.game_display_name) {
            if (c == ' ') norm += '_';
            else norm += c;
        }
        if (norm == game_id) return p.game_id;
    }

    return game_id;  // no match - return as-is
}

}  // namespace engine
