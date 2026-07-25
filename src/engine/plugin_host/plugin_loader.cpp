#include "engine/plugin_host/plugin_loader.h"
#include "engine/log/logger.h"

#include "gmm_abi_v1.h"

#include <dlfcn.h>
#include <filesystem>

namespace engine {

// Bridging context passed as user_data to plugin registration callbacks
struct RegistrationBridge {
    PluginLoader* loader = nullptr;
    PluginInfo* current_plugin = nullptr;
};

// ABI callback implementations
static void cb_register_identity(GmmRegistrationCtx* ctx,
                                  uint32_t steam_appid,
                                  const char* gog_id,
                                  const char* epic_namespace,
                                  const char* nexus_domain,
                                  const char* exe_windows,
                                  const char* exe_linux,
                                  const char* exe_macos) {
    auto* bridge = static_cast<RegistrationBridge*>(ctx->user_data);
    if (!bridge || !bridge->current_plugin) return;

    bridge->current_plugin->steam_appid = steam_appid;
    bridge->current_plugin->nexus_domain = nexus_domain ? nexus_domain : "";

    Logger::instance().info("Plugin registered identity: appid=" +
        std::to_string(steam_appid) +
        " nexus=" + (nexus_domain ? std::string(nexus_domain) : "none"));
}

static void cb_register_stage_claim(GmmRegistrationCtx* ctx,
                                     const char* stage_name,
                                     GmmStageFn fn,
                                     int priority) {
    auto* bridge = static_cast<RegistrationBridge*>(ctx->user_data);
    if (!bridge || !bridge->current_plugin) return;

    std::string game_id = bridge->current_plugin->game_id;
    std::string stage = stage_name ? stage_name : "";

    bridge->loader->stage_registry().register_claim(
        game_id, stage,
        [fn, game_id, stage](Mod& mod, PipelineContext& ctx_) -> bool {
            // Bridge from C++ types to ABI types
            (void)mod; (void)ctx_;
            // TODO: Create GmmModHandle/GmmInstanceHandle wrappers
            return fn(nullptr, nullptr, nullptr, nullptr, nullptr);
        },
        priority, bridge->current_plugin->path);
}

static void cb_register_order_encoding(GmmRegistrationCtx* ctx,
                                        GmmOrderEncodingFn fn) {
    auto* bridge = static_cast<RegistrationBridge*>(ctx->user_data);
    if (!bridge) return;

    // TODO: Store order encoding callback in plugin info for pipeline use
    Logger::instance().info("Plugin registered order encoding hook");
    (void)fn;
}

static void cb_register_deploy_strategy(GmmRegistrationCtx* ctx,
                                         GmmDeployFn deploy_fn,
                                         GmmRemoveFn remove_fn) {
    auto* bridge = static_cast<RegistrationBridge*>(ctx->user_data);
    if (!bridge) return;

    Logger::instance().info("Plugin registered deploy strategy");
    (void)deploy_fn; (void)remove_fn;
}

static void cb_register_tool(GmmRegistrationCtx* ctx,
                              const char* tool_id,
                              const char* kind,
                              void (*invoke_fn)(void*),
                              void* user_data) {
    auto* bridge = static_cast<RegistrationBridge*>(ctx->user_data);
    if (!bridge) return;

    Logger::instance().info("Plugin registered tool: " +
        std::string(tool_id ? tool_id : "unknown") +
        " (" + (kind ? kind : "unknown") + ")");
    (void)invoke_fn; (void)user_data;
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

PluginLoader::~PluginLoader() {
    unload_all();
}

bool PluginLoader::load_plugin(const std::string& path) {
    if (is_loaded(path)) {
        Logger::instance().warn("Plugin already loaded: " + path);
        return true;
    }

    Logger::instance().info("Loading plugin: " + path);

    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        Logger::instance().error("Failed to load plugin: " + path + " — " + dlerror());
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
    info.abi_version = plugin_abi;
    info.loaded = true;
    info.handle = handle;

    // Derive display name from game_id
    static const std::unordered_map<std::string, std::string> display_names = {
        {"skyrimse", "Skyrim Special Edition"},
        {"isaac", "The Binding of Isaac: Rebirth"},
    };
    auto name_it = display_names.find(info.game_id);
    info.game_display_name = (name_it != display_names.end())
        ? name_it->second : info.game_id;

    // Set up registration context and call plugin
    GmmRegistrationCtx ctx = {};
    ctx.register_identity = cb_register_identity;
    ctx.register_stage_claim = cb_register_stage_claim;
    ctx.register_order_encoding = cb_register_order_encoding;
    ctx.register_deploy_strategy = cb_register_deploy_strategy;
    ctx.register_tool = cb_register_tool;
    ctx.register_capability = cb_register_capability;

    RegistrationBridge bridge;
    bridge.loader = this;
    bridge.current_plugin = &info;
    ctx.user_data = &bridge;

    register_fn(&ctx);

    info.registered = true;
    plugins_.push_back(info);

    Logger::instance().info("Plugin registered: " + path +
        " (game=" + info.game_id +
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
    }

    Logger::instance().info("Loaded " + std::to_string(loaded) + " plugins from " + dir_path);
    return loaded > 0;
}

bool PluginLoader::is_loaded(const std::string& path) const {
    for (const auto& p : plugins_) {
        if (p.path == path) return true;
    }
    return false;
}

void PluginLoader::unload_all() {
    for (auto& p : plugins_) {
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
    return game_id;
}

}  // namespace engine
