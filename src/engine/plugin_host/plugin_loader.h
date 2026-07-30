#pragma once

#include "engine/registry/game_capabilities.h"
#include "engine/registry/stage_registry.h"
#include "engine/registry/hook_registry.h"
#include "engine/registry/game_knowledge.h"
#include "engine/tools/external_tool.h"

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

    // Register a plugin that was loaded externally (e.g. by Python loader)
    void add_loaded_plugin(PluginInfo info);

    const std::vector<PluginInfo>& plugins() const { return plugins_; }
    std::vector<PluginInfo>& plugins_mutable() { return plugins_; }

    bool is_loaded(const std::string& path) const;

    // Look up a game's display name by its game_id
    [[nodiscard]] std::string display_name_for(const std::string& game_id) const;

    // Resolve a possibly-legacy game_id to the canonical game_id registered
    // by a loaded plugin. Falls back to fuzzy matching (substring, instance-name
    // normalization) so old instances with shortnames survive plugin renames.
    [[nodiscard]] std::string resolve_game_id(const std::string& game_id) const;

    // Access registries populated by plugin registration
    StageRegistry& stage_registry() { return stage_registry_; }
    HookRegistry& hook_registry() { return hook_registry_; }
    GameCapabilities& capabilities() { return capabilities_; }
    ToolRegistry& tool_registry() { return tool_registry_; }
    GameKnowledge& knowledge() { return knowledge_; }

    const StageRegistry& stage_registry() const { return stage_registry_; }
    const HookRegistry& hook_registry() const { return hook_registry_; }
    const GameCapabilities& capabilities() const { return capabilities_; }
    const ToolRegistry& tool_registry() const { return tool_registry_; }
    const GameKnowledge& knowledge() const { return knowledge_; }

private:
    void* dlopen_handle(const std::string& path);
    void unload_all();

    std::vector<PluginInfo> plugins_;
    StageRegistry stage_registry_;
    HookRegistry hook_registry_;
    GameCapabilities capabilities_;
    ToolRegistry tool_registry_;
    GameKnowledge knowledge_;
};

}  // namespace engine
