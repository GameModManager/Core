#include "engine/pipeline/plugin_host/plugin_loader.h"
#include "engine/pipeline/plugin_host/category_factory.h"
#include "engine/pipeline/plugin_host/python_loader.h"
#include "engine/pipeline/plugin_host/diagnostics_registry.h"
#include "engine/pipeline/plugin_host/diagnose_registry.h"
#include "engine/pipeline/plugin_host/file_mapper_registry.h"
#include "engine/pipeline/plugin_host/requirements_registry.h"
#include "engine/core/events/event_bus.h"
#include "engine/core/log/logger.h"
#include "engine/mod/model/mod.h"
#include "engine/pipeline/fomod_stage.h"
#include "engine/pipeline/pipeline.h"
#include "engine/game/plugins/plugin_database.h"
#include "engine/game/registry/game_features/game_feature_registry.h"
#include "engine/game/registry/game_features/game_feature.h"
#include "engine/game/saves/skyrim_save.h"
#include "engine/game/saves/save_reader.h"
#include "engine/sort/sort_registry.h"
#include "engine/sort/abi_sort_provider.h"
#include "engine/pipeline/plugin_host/order_encoding_registry.h"
#include "engine/pipeline/plugin_host/deploy_strategy_registry.h"
#include "engine/pipeline/plugin_host/hook_registry.h"
#include "engine/pipeline/plugin_host/plugin_settings_registry.h"
#include "engine/pipeline/plugin_host/save_parser_registry.h"
#include "engine/pipeline/plugin_host/tool_registry.h"

#include "gmm_abi_v1.h"
#include "gmm_abi_v2.h"

// v2 IPluginPreview backing store. Header-only + Qt-free so the engine can
// populate it without linking the UI library (keeps gmm_engine Qt-free and
// avoids an engine->ui link cycle). The UI casts the returned void* to QWidget*.
#include "ui/preview/preview_registry.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <optional>
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
    // Only the first plugin to register a given game_id gets game_support = true;
    // a secondary registrant (e.g. a Tool plugin that also calls register_identity
    // to scope its sort provider) still gets its game_id set but doesn't appear in
    // the "Create New Instance" game list.
    const std::string gid = bridge->current_plugin->game_id;
    bool duplicate = false;
    if (bridge->loader) {
        for (const auto& other : bridge->loader->plugins()) {
            if (&other == bridge->current_plugin) continue;
            if (other.game_support && other.game_id == gid) {
                duplicate = true;
                break;
            }
        }
    }
    if (!duplicate) {
        bridge->current_plugin->game_support = true;
    }

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

static void cb_register_categories(GmmRegistrationCtx* ctx, const int* ids,
                                   const char* const* names,
                                   const int* parent_ids, size_t count) {
    if (!ids || !names || count == 0) return;

    CategoryFactory::instance().merge(ids, names, parent_ids, count);

    auto* bridge = static_cast<RegistrationBridge*>(ctx->user_data);
    if (bridge && bridge->current_plugin) {
        Logger::instance().debug(
            "Plugin registered " + std::to_string(count) +
            " categories (plugin=" + bridge->current_plugin->game_id + ")");
    }
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

    // Persist the declaration in the process-wide settings registry so the host
    // callbacks can read/write these keys at runtime (v2 parity for v1 plugins).
    const std::string basename =
        std::filesystem::path(bridge->current_plugin->path).filename().string();
    PluginSettingsRegistry::instance().register_settings(basename, keys, values, count);
    PluginSettingsRegistry::instance().register_alias(bridge->current_plugin->game_id, basename);
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

    // Persist the declaration in the process-wide settings registry so the host
    // callbacks can read/write these keys at runtime (v2 parity for v1 plugins).
    const std::string basename =
        std::filesystem::path(bridge->current_plugin->path).filename().string();
    PluginSettingsRegistry::instance().register_settings_tab(
        basename, title, keys, types, defaults, options, count);
    PluginSettingsRegistry::instance().register_alias(bridge->current_plugin->game_id, basename);
}

static void cb_register_diagnostics(GmmRegistrationCtx* ctx,
                                    GmmDiagnosticsFn fn,
                                    void* user_data) {
    auto* bridge = static_cast<RegistrationBridge*>(ctx->user_data);
    if (!bridge || !bridge->current_plugin) return;

    DiagnosticsRegistry::instance().register_provider(
        bridge->current_plugin->game_id, fn, user_data);
}

static void cb_register_save_parser(GmmRegistrationCtx* ctx,
                                        const char* game_id,
                                        GmmSaveParserFn fn,
                                        int priority,
                                        void* user_data) {
    auto* bridge = static_cast<RegistrationBridge*>(ctx->user_data);
    if (!bridge || !bridge->current_plugin) return;

    std::string gid = game_id ? game_id : bridge->current_plugin->game_id;
    if (gid.empty() || !fn) {
        Logger::instance().warn("Save parser registered with empty game_id or null fn");
        return;
    }

    std::string source = bridge->current_plugin->path;
    SaveParserRegistry::instance().register_parser(
        gid, priority,
        [fn, user_data](const std::filesystem::path& path,
                        const std::string& game_id) -> SaveGame {
            GmmSaveGameC c_out = {};
            if (!fn(path.string().c_str(), game_id.c_str(), &c_out, user_data)) {
                throw SaveParseError("plugin parser returned 0");
            }
            SaveGame out;
            out.file_path = c_out.file_path ? c_out.file_path : "";
            out.game_id = c_out.game_id ? c_out.game_id : game_id;
            out.creation_time = c_out.creation_time;
            out.pc_name = c_out.pc_name ? c_out.pc_name : "";
            out.pc_level = c_out.pc_level;
            out.pc_location = c_out.pc_location ? c_out.pc_location : "";
            out.save_number = c_out.save_number;
            for (uint32_t i = 0; i < c_out.plugin_count && i < GMM_SAVE_MAX_PLUGINS; ++i) {
                out.plugins.push_back(c_out.plugins[i] ? c_out.plugins[i] : "");
                free(c_out.plugins[i]);
            }
            for (uint32_t i = 0; i < c_out.light_plugin_count && i < GMM_SAVE_MAX_PLUGINS; ++i) {
                out.light_plugins.push_back(c_out.light_plugins[i] ? c_out.light_plugins[i] : "");
                free(c_out.light_plugins[i]);
            }
            free(c_out.file_path);
            free(c_out.game_id);
            free(c_out.pc_name);
            free(c_out.pc_location);
            return out;
        },
        user_data, source);
    Logger::instance().debug("Plugin registered save parser for game=" + gid);
}

static void cb_register_animation_parser(GmmRegistrationCtx* ctx,
                                         const char* game_id,
                                         const char* file_extension,
                                         GmmAnimationParserFn fn,
                                         int priority,
                                         void* user_data) {
    auto* bridge = static_cast<RegistrationBridge*>(ctx->user_data);
    if (!bridge || !bridge->current_plugin) return;

    // A NULL game_id means the parser is non-game-specific (file-format based,
    // applies to every game) and is registered under the empty/global game_id.
    // This is what lets a generic plugin such as Anm2Support serve any game
    // without being re-registered per game. An explicit game_id scopes the
    // parser to that one game (and overrides any global parser for it).
    std::string gid = game_id ? game_id : "";
    if (!fn) {
        Logger::instance().warn("Animation parser registered with null fn");
        return;
    }

    std::string source = bridge->current_plugin->path;
    auto feature = std::make_shared<AnimationParserFeature>(
        [fn, user_data](const std::string& file_path,
                        const std::string& base_dir)
            -> std::optional<AnimationParserFeature::AnimationData> {
            GmmAnimationDataC c_out = {};
            if (!fn(file_path.c_str(), base_dir.c_str(), &c_out, user_data)) {
                return std::nullopt;
            }
            AnimationParserFeature::AnimationData data;
            data.fps = c_out.fps;
            data.canvas_width = c_out.canvas_width;
            data.canvas_height = c_out.canvas_height;
            for (size_t fi = 0; fi < c_out.frame_count; ++fi) {
                auto& cf = c_out.frames[fi];
                AnimationParserFeature::Frame frame;
                frame.delay_ms = cf.delay_ms;
                for (size_t li = 0; li < cf.layer_count; ++li) {
                    auto& cl = cf.layers[li];
                    AnimationParserFeature::LayerItem layer;
                    layer.x = cl.x;
                    layer.y = cl.y;
                    layer.width = cl.width;
                    layer.height = cl.height;
                    if (cl.rgba_pixels && cl.pixel_count > 0) {
                        layer.rgba_pixels.assign(cl.rgba_pixels,
                            cl.rgba_pixels + cl.pixel_count);
                        free(cl.rgba_pixels);
                    }
                    frame.layers.push_back(std::move(layer));
                }
                data.frames.push_back(std::move(frame));
                free(cf.layers);
            }
            free(c_out.frames);
            return data;
        });

    GameFeatureRegistry::instance().register_feature(
        gid, "animation_parser", priority, std::move(feature), source);
    Logger::instance().debug("Plugin registered animation parser for game=" + gid);
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

/* v2 wrapper: casts void* back to GmmModHandle for the v1 cb_fomod_wizard */
static int cb_fomod_wizard_v2(void* mod, char* out_json, size_t out_capacity) {
    return cb_fomod_wizard(static_cast<GmmModHandle>(mod), out_json, out_capacity);
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

static void cb_register_wildcard_stage_claim(GmmRegistrationCtx* ctx,
                                              const char* game_id,
                                              const char* stage_name,
                                              GmmStageFn fn,
                                              int priority) {
    auto* bridge = static_cast<RegistrationBridge*>(ctx->user_data);
    if (!bridge || !bridge->current_plugin) return;

    // NULL/empty game_id = wildcard (applies to all games)
    std::string gid = game_id ? game_id : "";
    std::string stage = stage_name ? stage_name : "";
    if (stage.empty() || !fn) return;

    bridge->loader->stage_registry().register_claim(
        gid, stage,
        [fn](Mod& mod, PipelineContext& ctx_) -> bool {
            GmmModHandle mod_h = reinterpret_cast<GmmModHandle>(&mod);
            GmmInstanceHandle inst_h =
                reinterpret_cast<GmmInstanceHandle>(ctx_.instance);
            GmmConflictIndexHandle conf_h =
                reinterpret_cast<GmmConflictIndexHandle>(ctx_.conflict_index);
            GmmProfileHandle prof_h =
                reinterpret_cast<GmmProfileHandle>(ctx_.profile);
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

    // Register the hook function into HookRegistry so the engine can fire
    // it at pipeline points (before_deploy, after_scan, conflict_resolution).
    // Note: GmmHookFn expects void* data, so we const_cast the string data.
    bridge->loader->hook_registry().register_hook(
        hook_tag,
        [fn, user_data, hook_tag, hook_data](Mod& mod, PipelineContext& ctx) {
            (void)mod; (void)ctx;
            if (fn) fn(hook_tag.c_str(), const_cast<char*>(hook_data.c_str()), user_data);
        },
        priority,
        bridge->current_plugin->path);

    Logger::instance().debug("Plugin registered knowledge: " + hook_tag +
        " (game=" + game_id + ", data=" + hook_data + ")");
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

    // Plugins that never called register_game have game_support=false — register their tools as global
    if (!bridge->current_plugin->game_support) {
        tool.game_id = "";
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

// ---------------------------------------------------------------------------
// v2 ABI callbacks
//
// These mirror the v1 RegistrationBridge callbacks but operate on the v2
// GmmRegistrationCtxV2 and store into the same internal registries (or, for
// v2-only concepts with no v1 equivalent, onto PluginInfo). The v1 path is
// left untouched; the two sets of callbacks share only the RegistrationBridge
// plumbing (loader + current_plugin).
// ---------------------------------------------------------------------------

static void cb_v2_register_plugin(GmmRegistrationCtxV2* ctx, GmmPluginInfo info) {
    auto* bridge = static_cast<RegistrationBridge*>(ctx->user_data);
    if (!bridge || !bridge->current_plugin) return;

    if (info.name) bridge->current_plugin->plugin_name = info.name;
    if (info.author) bridge->current_plugin->author = info.author;
    if (info.version) bridge->current_plugin->version = info.version;
    if (info.description) bridge->current_plugin->description = info.description;

    // Map the plugin's own name to its basename so host_get_setting(plugin_name,
    // key) resolves to the same QSettings store the UI persists under.
    if (info.name) {
        const std::string basename =
            std::filesystem::path(bridge->current_plugin->path).filename().string();
        PluginSettingsRegistry::instance().register_alias(info.name, basename);
    }
}

static void cb_v2_register_settings(GmmRegistrationCtxV2* ctx,
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

    // Persist the declaration in the process-wide settings registry so the host
    // callbacks can read/write these keys at runtime.
    const std::string basename =
        std::filesystem::path(bridge->current_plugin->path).filename().string();
    PluginSettingsRegistry::instance().register_settings(basename, keys, values, count);
    PluginSettingsRegistry::instance().register_alias(bridge->current_plugin->game_id, basename);
    PluginSettingsRegistry::instance().register_alias(bridge->current_plugin->plugin_name, basename);
}

static void cb_v2_register_settings_tab(GmmRegistrationCtxV2* ctx,
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

    // Persist the declaration in the process-wide settings registry so the host
    // callbacks can read/write these keys at runtime.
    const std::string basename =
        std::filesystem::path(bridge->current_plugin->path).filename().string();
    PluginSettingsRegistry::instance().register_settings_tab(
        basename, title, keys, types, defaults, options, count);
    PluginSettingsRegistry::instance().register_alias(bridge->current_plugin->game_id, basename);
    PluginSettingsRegistry::instance().register_alias(bridge->current_plugin->plugin_name, basename);
}

static void cb_v2_register_requirements(GmmRegistrationCtxV2* ctx,
                                         GmmRequirementsFn fn,
                                         void* user_data) {
    auto* bridge = static_cast<RegistrationBridge*>(ctx->user_data);
    if (!bridge || !bridge->current_plugin || !fn) return;

    // Defer evaluation: store the provider in the process-wide registry and
    // let the loader run check_requirements() once every plugin is loaded, so
    // cross-plugin / cross-game dependencies can be resolved correctly (a
    // requirement declared by plugin A may only be satisfiable by plugin B,
    // which loads later). The provider is keyed by plugin path so it can be
    // dropped on unload before dlclose.
    RequirementsRegistry::instance().register_requirements(
        bridge->current_plugin->path, fn, user_data);
}

static void cb_v2_register_diagnostics(GmmRegistrationCtxV2* ctx,
                                       const char* game_id,
                                       GmmDiagnoseFn fn,
                                       void* user_data) {
    auto* bridge = static_cast<RegistrationBridge*>(ctx->user_data);
    if (!bridge || !bridge->current_plugin || !fn) return;

    std::string gid = game_id ? game_id : bridge->current_plugin->game_id;
    PluginDiagnostics d;
    d.game_id = gid;
    d.fn = reinterpret_cast<void*>(fn);
    d.user_data = user_data;
    bridge->current_plugin->diagnostics_v2.push_back(std::move(d));

    // Register into the process-wide v2 DiagnoseRegistry so the engine can
    // collect problems for this game via DiagnoseRegistry::collect_diagnostics.
    DiagnoseRegistry::instance().register_diagnostics(
        gid, fn, user_data, bridge->current_plugin->path);

    Logger::instance().debug("Plugin registered v2 diagnostics for game=" + gid);
}

static void cb_v2_register_game(GmmRegistrationCtxV2* ctx, GmmGameInfo info) {
    auto* bridge = static_cast<RegistrationBridge*>(ctx->user_data);
    if (!bridge || !bridge->current_plugin) return;

    if (info.game_id) bridge->current_plugin->game_id = info.game_id;
    if (info.display_name) bridge->current_plugin->game_display_name = info.display_name;
    bridge->current_plugin->steam_appid = info.steam_appid;
    if (info.nexus_domain) bridge->current_plugin->nexus_domain = info.nexus_domain;
    if (info.gog_id) bridge->current_plugin->gog_id = info.gog_id;
    if (info.epic_namespace) bridge->current_plugin->epic_namespace = info.epic_namespace;
    if (info.exe_windows) bridge->current_plugin->exe_windows = info.exe_windows;
    if (info.exe_linux) bridge->current_plugin->exe_linux = info.exe_linux;
    if (info.exe_macos) bridge->current_plugin->exe_macos = info.exe_macos;

    // register_game is THE game-support marker for v2 (MO2 IPluginGame parity).
    // Only the first plugin to register a given game_id gets game_support = true;
    // secondary registrants (e.g. a Tool plugin that also calls register_game to
    // scope its sort provider) still get their game_id set but don't appear in
    // the "Create New Instance" game list.
    const std::string gid = info.game_id ? info.game_id : "";
    bool duplicate = false;
    if (bridge->loader) {
        for (const auto& other : bridge->loader->plugins()) {
            if (&other == bridge->current_plugin) continue;
            if (other.game_support && other.game_id == gid) {
                duplicate = true;
                break;
            }
        }
    }
    if (!duplicate) {
        bridge->current_plugin->game_support = true;
    }

    Logger::instance().debug("Plugin registered game: id=" +
        bridge->current_plugin->game_id +
        " name=" + bridge->current_plugin->game_display_name +
        " nexus=" + (info.nexus_domain ? std::string(info.nexus_domain) : "none"));
}

static void cb_v2_register_game_feature(GmmRegistrationCtxV2* ctx,
                                        const char* feature_type,
                                        int priority,
                                        const char* const* folder_names,
                                        size_t folder_count,
                                        const char* const* file_extensions,
                                        size_t extension_count) {
    auto* bridge = static_cast<RegistrationBridge*>(ctx->user_data);
    if (!bridge || !bridge->current_plugin) return;

    // v2 has no explicit game_id on this call; scope to the plugin's own game.
    std::string gid = bridge->current_plugin->game_id;
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

static void cb_v2_register_game_feature_data(GmmRegistrationCtxV2* ctx,
                                             const char* feature_type,
                                             int priority,
                                             const char* const* keys,
                                             const char* const* values,
                                             size_t count) {
    auto* bridge = static_cast<RegistrationBridge*>(ctx->user_data);
    if (!bridge || !bridge->current_plugin) return;

    std::string gid = bridge->current_plugin->game_id;
    std::string type = feature_type ? feature_type : "";
    if (gid.empty() || type.empty()) {
        Logger::instance().warn("Game feature data registered with empty game_id/type");
        return;
    }

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

static void cb_v2_register_hook(GmmRegistrationCtxV2* ctx,
                                const char* tag,
                                const char* data,
                                GmmHookFnV2 fn,
                                int priority,
                                void* user_data) {
    auto* bridge = static_cast<RegistrationBridge*>(ctx->user_data);
    if (!bridge || !bridge->current_plugin) return;

    std::string game_id = bridge->current_plugin->game_id;
    std::string hook_tag = tag ? tag : "";
    std::string hook_data = data ? data : "";

    // Store as game knowledge (data payload) — the v1 path's behaviour.
    bridge->loader->knowledge().set(game_id, hook_tag, hook_data);

    // Register the hook function into the instance-based engine::HookRegistry
    // (v1-style) so the engine can fire it at pipeline points
    // (before_deploy, after_scan, conflict_resolution) through the existing
    // fire() path. The hook fires with the tag + data + user_data the plugin
    // provided.
    bridge->loader->hook_registry().register_hook(
        hook_tag,
        [fn, user_data, hook_tag, hook_data](Mod& mod, PipelineContext& ctx) {
            (void)mod; (void)ctx;
            if (fn) fn(hook_tag.c_str(), (void*)hook_data.data(), user_data);
        },
        priority,
        bridge->current_plugin->path);

    // Also register the raw v2 ABI hook into the v2 HookRegistry singleton so
    // the pipeline can dispatch it directly via HookRegistry::instance().dispatch()
    // with an arbitrary data pointer (the v2 GmmHookFnV2 contract).
    ::HookRegistry::instance().register_hook(
        hook_tag.c_str(), fn, priority, user_data,
        bridge->current_plugin->path.c_str());

    Logger::instance().debug("Plugin registered knowledge: " + hook_tag +
        " (game=" + game_id + ", data=" + hook_data + ")");
}

static void cb_v2_register_order_encoding(GmmRegistrationCtxV2* ctx,
                                            GmmOrderEncodingFnV2 fn,
                                            void* user_data) {
    auto* bridge = static_cast<RegistrationBridge*>(ctx->user_data);
    if (!bridge || !bridge->current_plugin) return;

    const std::string& game_id = bridge->current_plugin->game_id;
    bridge->current_plugin->order_encoding_fn = fn;
    bridge->current_plugin->order_encoding_user_data = user_data;

    // Store the callback in the order-encoding registry so the pipeline can
    // retrieve and call it when writing load-order files (plugins.txt, ...).
    OrderEncodingRegistry::instance().register_provider(
        game_id, fn, user_data, bridge->current_plugin->path);

    Logger::instance().debug("Plugin registered v2 order encoding hook (game=" +
        game_id + ")");
}

static void cb_v2_register_deploy_strategy(GmmRegistrationCtxV2* ctx,
                                             GmmDeployFnV2 deploy_fn,
                                             GmmRemoveFnV2 remove_fn,
                                             void* user_data) {
    auto* bridge = static_cast<RegistrationBridge*>(ctx->user_data);
    if (!bridge || !bridge->current_plugin) return;

    const std::string& game_id = bridge->current_plugin->game_id;
    bridge->current_plugin->deploy_fn = deploy_fn;
    bridge->current_plugin->remove_fn = remove_fn;
    bridge->current_plugin->deploy_user_data = user_data;

    // Store the callbacks in the deploy-strategy registry so the pipeline can
    // retrieve and call them instead of the built-in DeploymentStrategy.
    DeployStrategyRegistry::instance().register_provider(
        game_id, deploy_fn, remove_fn, user_data, bridge->current_plugin->path);

    Logger::instance().debug("Plugin registered v2 deploy strategy (game=" +
        game_id + ")");
}

static void cb_v2_register_file_mapper(GmmRegistrationCtxV2* ctx,
                                       const char* game_id,
                                       GmmFileMapperFn fn,
                                       void* user_data) {
    auto* bridge = static_cast<RegistrationBridge*>(ctx->user_data);
    if (!bridge || !bridge->current_plugin || !fn) return;

    std::string gid = game_id ? game_id : bridge->current_plugin->game_id;
    PluginFileMapper m;
    m.game_id = gid;
    m.fn = reinterpret_cast<void*>(fn);
    m.user_data = user_data;
    bridge->current_plugin->file_mappers.push_back(std::move(m));

    // Also register into the process-wide FileMapperRegistry so the deploy
    // pipeline can aggregate virtual file overlays across all plugins via
    // FileMapperRegistry::instance().get_mappings(game_id). The registry stores
    // the raw GmmFileMapperFn + user_data and drops it on unload (clear_plugin),
    // so the pointer never outlives the .so that owns fn.
    FileMapperRegistry::instance().register_mapper(
        gid, fn, user_data, bridge->current_plugin->path);

    Logger::instance().debug("Plugin registered v2 file mapper for game=" + gid);
}

static void cb_v2_register_sort_provider(GmmRegistrationCtxV2* ctx,
                                         GmmSortFn sort_fn,
                                         void* user_data) {
    auto* bridge = static_cast<RegistrationBridge*>(ctx->user_data);
    if (!bridge) return;

    std::string gid = bridge->current_plugin ? bridge->current_plugin->game_id : "";
    if (gid.empty()) {
        Logger::instance().warn("Sort provider registered with empty game_id");
        return;
    }

    auto provider = std::make_unique<AbiSortProvider>(gid.c_str(), sort_fn, user_data);
    SortRegistry::instance().register_provider(gid, std::move(provider));

    Logger::instance().debug("Plugin registered v2 sort provider for game=" + gid);
}

static void cb_v2_register_stage_claim(GmmRegistrationCtxV2* ctx,
                                       const char* stage_name,
                                       GmmStageFnV2 fn,
                                       int priority) {
    auto* bridge = static_cast<RegistrationBridge*>(ctx->user_data);
    if (!bridge || !bridge->current_plugin) return;

    std::string game_id = bridge->current_plugin->game_id;
    std::string stage = stage_name ? stage_name : "";
    if (stage.empty() || !fn) return;

    bridge->loader->stage_registry().register_claim(
        game_id, stage,
        [fn](Mod& mod, PipelineContext& ctx_) -> bool {
            GmmModHandle mod_h = reinterpret_cast<GmmModHandle>(&mod);
            GmmInstanceHandle inst_h =
                reinterpret_cast<GmmInstanceHandle>(ctx_.instance);
            GmmConflictIndexHandle conf_h =
                reinterpret_cast<GmmConflictIndexHandle>(ctx_.conflict_index);
            GmmProfileHandle prof_h =
                reinterpret_cast<GmmProfileHandle>(ctx_.profile);
            g_active_stage_ctx = &ctx_;
            const int result = fn(mod_h, inst_h, conf_h, prof_h, nullptr);
            g_active_stage_ctx = nullptr;
            return result != 0;
        },
        priority, bridge->current_plugin->path);
}

static void cb_v2_register_wildcard_stage_claim(GmmRegistrationCtxV2* ctx,
                                                const char* game_id,
                                                const char* stage_name,
                                                GmmStageFnV2 fn,
                                                int priority) {
    auto* bridge = static_cast<RegistrationBridge*>(ctx->user_data);
    if (!bridge || !bridge->current_plugin) return;

    std::string gid = game_id ? game_id : "";
    std::string stage = stage_name ? stage_name : "";
    if (stage.empty() || !fn) return;

    bridge->loader->stage_registry().register_claim(
        gid, stage,
        [fn](Mod& mod, PipelineContext& ctx_) -> bool {
            GmmModHandle mod_h = reinterpret_cast<GmmModHandle>(&mod);
            GmmInstanceHandle inst_h =
                reinterpret_cast<GmmInstanceHandle>(ctx_.instance);
            GmmConflictIndexHandle conf_h =
                reinterpret_cast<GmmConflictIndexHandle>(ctx_.conflict_index);
            GmmProfileHandle prof_h =
                reinterpret_cast<GmmProfileHandle>(ctx_.profile);
            g_active_stage_ctx = &ctx_;
            const int result = fn(mod_h, inst_h, conf_h, prof_h, nullptr);
            g_active_stage_ctx = nullptr;
            return result != 0;
        },
        priority, bridge->current_plugin->path);
}

static void cb_v2_register_preview(GmmRegistrationCtxV2* ctx,
                                   GmmPreviewInfo info,
                                   GmmPreviewFn fn,
                                   void* user_data) {
    auto* bridge = static_cast<RegistrationBridge*>(ctx->user_data);
    if (!bridge || !bridge->current_plugin || !fn) return;

    PluginPreview p;
    p.file_extension = info.file_extension ? info.file_extension : "";
    p.preview_data = info.preview_data;
    p.fn = reinterpret_cast<void*>(fn);
    p.user_data = user_data;
    bridge->current_plugin->previews.push_back(std::move(p));

    // Mirror the registration into the UI-side PreviewRegistry so the preview
    // window can embed the plugin-provided QWidget* for this extension. The
    // registry stores the opaque fn + user_data; the engine never sees QWidget.
    ui::preview::PreviewRegistry::instance().register_preview(
        p.file_extension, fn, info.preview_data, user_data,
        bridge->current_plugin->path);

    Logger::instance().debug("Plugin registered v2 preview for extension=" +
        p.file_extension);
}

static void cb_v2_register_tool(GmmRegistrationCtxV2* ctx,
                                const char* tool_id,
                                const char* kind,
                                GmmToolInvokeFn fn,
                                void* user_data) {
    auto* bridge = static_cast<RegistrationBridge*>(ctx->user_data);
    if (!bridge || !bridge->current_plugin) return;

    ExternalTool tool;
    tool.tool_id = tool_id ? tool_id : "";
    tool.game_id = bridge->current_plugin->game_id;
    tool.display_name = tool.tool_id;

    std::string kind_str = kind ? kind : "advisory";
    tool.kind = (kind_str == "workshop") ? ToolKind::Workshop : ToolKind::Advisory;

    if (fn) {
        tool.invoke_fn = [fn](void* ud) { fn(ud); };
        tool.invoke_user_data = user_data;
    }

    // Plugins that never called register_game have game_support=false — register their tools as global
    if (!bridge->current_plugin->game_support) {
        tool.game_id = "";
    }

    bridge->loader->tool_registry().register_tool(tool);

    // Also register into the v2 IPluginTool registry — the canonical store of
    // plugin-provided tool callbacks (raw fn + user_data + owning plugin path,
    // used for unload cleanup). The platform/tools ToolRegistry above drives the
    // Tools menu; this one is the v2 source of truth.
    PluginToolRegistry::instance().register_tool(
        tool.tool_id, kind_str, fn, user_data,
        bridge->current_plugin->path);

    Logger::instance().debug("Plugin registered v2 tool: " + tool.tool_id +
        " (" + kind_str + ") for game=" + tool.game_id);
}

static void cb_v2_register_modpage(GmmRegistrationCtxV2* ctx,
                                   const char* url,
                                   GmmModPageDownloadFn fn,
                                   void* user_data) {
    auto* bridge = static_cast<RegistrationBridge*>(ctx->user_data);
    if (!bridge || !bridge->current_plugin || !fn) return;

    PluginModPage m;
    m.url = url ? url : "";
    m.fn = reinterpret_cast<void*>(fn);
    m.user_data = user_data;
    bridge->current_plugin->modpages.push_back(std::move(m));

    Logger::instance().debug("Plugin registered v2 modpage for url=" + m.url);
}

static void cb_v2_register_save_parser(GmmRegistrationCtxV2* ctx,
                                       const char* game_id,
                                       GmmSaveParserFnV2 fn,
                                       int priority,
                                       void* user_data) {
    auto* bridge = static_cast<RegistrationBridge*>(ctx->user_data);
    if (!bridge || !bridge->current_plugin) return;

    std::string gid = game_id ? game_id : bridge->current_plugin->game_id;
    if (gid.empty() || !fn) {
        Logger::instance().warn("Save parser registered with empty game_id or null fn");
        return;
    }

    std::string source = bridge->current_plugin->path;
    SaveParserRegistry::instance().register_parser(
        gid, priority,
        [fn, user_data](const std::filesystem::path& path,
                        const std::string& game_id) -> SaveGame {
            GmmSaveDataV2 c_out = {};
            if (!fn(path.string().c_str(), game_id.c_str(), &c_out, user_data)) {
                throw SaveParseError("plugin v2 parser returned 0");
            }
            SaveGame out;
            out.file_path = c_out.file_path ? c_out.file_path : "";
            out.game_id = c_out.game_id ? c_out.game_id : game_id;
            out.creation_time = c_out.creation_time;
            out.pc_name = c_out.pc_name ? c_out.pc_name : "";
            out.pc_level = c_out.pc_level;
            out.pc_location = c_out.pc_location ? c_out.pc_location : "";
            out.save_number = c_out.save_number;
            for (uint32_t i = 0; i < c_out.plugin_count && i < 256; ++i) {
                out.plugins.push_back(c_out.plugins[i] ? c_out.plugins[i] : "");
                free(c_out.plugins[i]);
            }
            for (uint32_t i = 0; i < c_out.light_plugin_count && i < 256; ++i) {
                out.light_plugins.push_back(c_out.light_plugins[i] ? c_out.light_plugins[i] : "");
                free(c_out.light_plugins[i]);
            }
            free(c_out.file_path);
            free(c_out.game_id);
            free(c_out.pc_name);
            free(c_out.pc_location);
            return out;
        },
        user_data, source);
    Logger::instance().debug("Plugin registered v2 save parser for game=" + gid);
}

static void cb_v2_register_category(GmmRegistrationCtxV2* ctx,
                                    const char* category) {
    auto* bridge = static_cast<RegistrationBridge*>(ctx->user_data);
    if (!bridge || !bridge->current_plugin) return;

    if (category) bridge->current_plugin->category = category;
}

static void cb_v2_register_categories(GmmRegistrationCtxV2* ctx, const int* ids,
                                      const char* const* names,
                                      const int* parent_ids, size_t count) {
    if (!ids || !names || count == 0) return;

    CategoryFactory::instance().merge(ids, names, parent_ids, count);

    auto* bridge = static_cast<RegistrationBridge*>(ctx->user_data);
    if (bridge && bridge->current_plugin) {
        Logger::instance().debug(
            "Plugin registered " + std::to_string(count) +
            " categories (plugin=" + bridge->current_plugin->game_id + ")");
    }
}

static void cb_v2_register_tab(GmmRegistrationCtxV2* ctx,
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

    if (plugin_abi == 1) {
        // ---- v1 path (unchanged) ----
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
        ctx.register_categories = cb_register_categories;
        ctx.register_settings = cb_register_settings;
        ctx.register_settings_tab = cb_register_settings_tab;
        ctx.register_diagnostics = cb_register_diagnostics;
        ctx.register_game_feature = cb_register_game_feature;
        ctx.register_game_feature_data = cb_register_game_feature_data;
        ctx.register_save_parser = cb_register_save_parser;
        ctx.register_animation_parser = cb_register_animation_parser;
        ctx.subscribe_event = cb_subscribe_event;
        ctx.host_ui.fomod_wizard = cb_fomod_wizard;
        ctx.register_wildcard_stage_claim = cb_register_wildcard_stage_claim;

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
    } else if (plugin_abi == 2) {
        // ---- v2 path ----
        auto register_fn = reinterpret_cast<void (*)(GmmRegistrationCtxV2*)>(
            dlsym(handle, "gmm_register_v2"));
        if (!register_fn) {
            Logger::instance().error("Plugin missing gmm_register_v2: " + path);
            dlclose(handle);
            return false;
        }

        PluginInfo info;
        info.path = path;
        info.game_id = std::filesystem::path(path).stem().string();
        info.game_display_name = info.game_id;  // fallback, overridden by register_game
        info.abi_version = plugin_abi;
        info.loaded = true;
        info.handle = handle;

        RegistrationBridge bridge;
        bridge.loader = this;
        bridge.current_plugin = &info;

        GmmRegistrationCtxV2 ctx = {};
        ctx.user_data = &bridge;
        ctx.register_plugin = cb_v2_register_plugin;
        ctx.register_settings = cb_v2_register_settings;
        ctx.register_settings_tab = cb_v2_register_settings_tab;
        ctx.register_requirements = cb_v2_register_requirements;
        ctx.register_diagnostics = cb_v2_register_diagnostics;
        ctx.register_game = cb_v2_register_game;
        ctx.register_game_feature = cb_v2_register_game_feature;
        ctx.register_game_feature_data = cb_v2_register_game_feature_data;
        ctx.register_hook = cb_v2_register_hook;
        ctx.register_order_encoding = cb_v2_register_order_encoding;
        ctx.register_deploy_strategy = cb_v2_register_deploy_strategy;
        ctx.register_file_mapper = cb_v2_register_file_mapper;
        ctx.register_sort_provider = cb_v2_register_sort_provider;
        ctx.register_stage_claim = cb_v2_register_stage_claim;
        ctx.register_wildcard_stage_claim = cb_v2_register_wildcard_stage_claim;
        ctx.register_preview = cb_v2_register_preview;
        ctx.register_tool = cb_v2_register_tool;
        ctx.register_modpage = cb_v2_register_modpage;
        ctx.register_save_parser = cb_v2_register_save_parser;
        ctx.register_category = cb_v2_register_category;
        ctx.register_categories = cb_v2_register_categories;
        ctx.register_tab = cb_v2_register_tab;
        ctx.host_ui.fomod_wizard = cb_fomod_wizard_v2;

        register_fn(&ctx);

        info.registered = true;
        plugins_.push_back(info);

        Logger::instance().debug("Plugin (v2) registered: " + info.game_display_name +
            " (" + path + ", game=" + info.game_id + ")");
        return true;
    } else {
        Logger::instance().error("ABI version mismatch: plugin=" +
            std::to_string(plugin_abi) + " host supports 1 and 2");
        dlclose(handle);
        return false;
    }
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

        // Platform-appropriate shared library extensions. CMake MODULE
        // libraries keep the ".so" suffix even on macOS, so Apple accepts
        // both spellings (dlopen loads either).
#if defined(_WIN32)
        if (ext == ".dll") {
            if (load_plugin(path.string())) loaded++;
        }
#else
        if (ext == ".so" || ext == ".dylib") {
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

    /* Register built-in save parsers for Skyrim games as fallbacks.
     * If a plugin already registered a parser (same game_id, same or higher
     * priority), the plugin's parser wins. These register at priority 0
     * (lowest) so any plugin override supersedes them. has_parser() guards
     * against re-adding on repeated load_directory calls (the builtins are
     * never cleared on unload, so they persist across reloads). */
    if (!SaveParserRegistry::instance().has_parser("skyrim")) {
        SaveParserRegistry::instance().register_parser(
            "skyrim", 0,
            [](const std::filesystem::path& path, const std::string&) {
                return parse_skyrim_save(path);
            },
            nullptr, "engine:builtin");
    }
    if (!SaveParserRegistry::instance().has_parser("skyrimse")) {
        SaveParserRegistry::instance().register_parser(
            "skyrimse", 0,
            [](const std::filesystem::path& path, const std::string& game_id) {
                return parse_skyrimse_save(path, game_id);
            },
            nullptr, "engine:builtin");
    }
    if (!SaveParserRegistry::instance().has_parser("skyrimvr")) {
        SaveParserRegistry::instance().register_parser(
            "skyrimvr", 0,
            [](const std::filesystem::path& path, const std::string& game_id) {
                return parse_skyrimse_save(path, game_id);
            },
            nullptr, "engine:builtin");
    }

    std::string list_str;
    for (size_t i = 0; i < plugins_.size(); ++i) {
        if (i > 0) list_str += ", ";
        list_str += plugins_[i].game_display_name;
    }
    Logger::instance().debug("Loaded plugins: [" + list_str + "]");

    // Evaluate plugin dependency requirements now that every plugin is loaded.
    // check_requirements() calls each registered provider and returns the
    // requirements that are not satisfied by the loaded plugin/game set.
    auto unmet = RequirementsRegistry::instance().check_requirements(plugins_);
    for (const auto& u : unmet) {
        std::string msg = u.message.empty()
            ? ("requires " + (u.type.empty() ? "dependency" : u.type) +
               (u.name.empty() ? "" : " '" + u.name + "'"))
            : u.message;
        Logger::instance().warn("Plugin requirement unmet (" + u.plugin_path +
            "): " + msg);
    }
    if (!unmet.empty()) {
        Logger::instance().warn(std::to_string(unmet.size()) +
            " plugin requirement(s) unmet across loaded plugins");
    }

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
        // Clear hooks registered by this plugin.
        hook_registry_.clear_plugin_hooks(p.path);
        // Clear v2 behavior-injection hooks registered by this plugin so the
        // v2 HookRegistry singleton never holds a dangling function pointer.
        ::HookRegistry::instance().clear_plugin(p.path.c_str());
        // Clear v2 order-encoding / deploy-strategy callbacks registered by
        // this plugin (dangling after dlclose).
        OrderEncodingRegistry::instance().clear_plugin(p.path);
        DeployStrategyRegistry::instance().clear_plugin(p.path);
        // Clear v2 preview generators registered by this plugin so the
        // PreviewRegistry never holds a dangling function pointer.
        ui::preview::PreviewRegistry::instance().clear_plugin(p.path);
        // Clear v2 diagnose providers registered by this plugin so the
        // DiagnoseRegistry never holds a dangling function pointer.
        DiagnoseRegistry::instance().clear_plugin(p.path);
        // Clear v2 file mappers registered by this plugin so the
        // FileMapperRegistry never holds a dangling function pointer.
        FileMapperRegistry::instance().clear_plugin(p.path);
        // Clear v2 requirement providers registered by this plugin so the
        // RequirementsRegistry never holds a dangling function pointer.
        RequirementsRegistry::instance().clear_plugin(p.path);
        // Clear save parsers registered by this plugin so the
        // SaveParserRegistry never holds a dangling ABI function pointer.
        SaveParserRegistry::instance().clear_plugin(p.path);
        // Clear v2 IPluginTool callbacks registered by this plugin so the
        // ToolRegistry never holds a dangling function pointer.
        PluginToolRegistry::instance().clear_plugin(p.path);
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
