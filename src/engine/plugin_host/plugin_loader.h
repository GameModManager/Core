#pragma once

#include "engine/registry/game_capabilities.h"
#include "engine/registry/stage_registry.h"
#include "engine/registry/hook_registry.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// ABI version from the header
#define GMM_ABI_VERSION 1

namespace engine {

struct PluginInfo {
    std::string path;
    std::string game_id;
    std::string game_display_name;  // e.g. "Skyrim Special Edition"
    uint32_t steam_appid = 0;
    std::string nexus_domain;
    uint32_t abi_version = 0;
    bool loaded = false;
    bool registered = false;
    void* handle = nullptr;  // dlopen handle
};

class PluginLoader {
public:
    PluginLoader() = default;
    ~PluginLoader();

    PluginLoader(const PluginLoader&) = delete;
    PluginLoader& operator=(const PluginLoader&) = delete;

    bool load_plugin(const std::string& path);
    bool load_directory(const std::string& dir_path);

    const std::vector<PluginInfo>& plugins() const { return plugins_; }

    bool is_loaded(const std::string& path) const;

    // Look up a game's display name by its game_id
    [[nodiscard]] std::string display_name_for(const std::string& game_id) const;

    // Access registries populated by plugin registration
    StageRegistry& stage_registry() { return stage_registry_; }
    HookRegistry& hook_registry() { return hook_registry_; }
    GameCapabilities& capabilities() { return capabilities_; }

    const StageRegistry& stage_registry() const { return stage_registry_; }
    const HookRegistry& hook_registry() const { return hook_registry_; }
    const GameCapabilities& capabilities() const { return capabilities_; }

private:
    void* dlopen_handle(const std::string& path);
    void unload_all();

    std::vector<PluginInfo> plugins_;
    StageRegistry stage_registry_;
    HookRegistry hook_registry_;
    GameCapabilities capabilities_;
};

}  // namespace engine
