#pragma once

#include "engine/game/registry/game_capabilities.h"
#include "engine/pipeline/registry/stage_registry.h"
#include "engine/pipeline/registry/hook_registry.h"
#include "engine/game/registry/game_knowledge.h"
#include "engine/platform/tools/external_tool.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Forward declare ABI image diff callback type
typedef void (*GmmImageDiffFn)(const char* const*, size_t, const char*, void*);

// A provider registered by a tool plugin for merging conflicting files
struct ImageDiffProvider {
    GmmImageDiffFn fn = nullptr;
    void* user_data = nullptr;
};

// ABI version from the header
#define GMM_ABI_VERSION 1

namespace engine {

class PluginDatabase;

struct PluginInfo {
    std::string path;
    std::string game_id;
    std::string game_display_name;  // e.g. "Skyrim Special Edition"
    std::string author;             // optional, via register_meta
    std::string version;            // optional, via register_meta
    std::string description;        // optional, via register_meta
    std::string category;           // optional, via register_category
    uint32_t steam_appid = 0;
    std::string nexus_domain;
    // True only when the plugin called register_identity — i.e. it provides
    // game support (a game to create instances for). Tool/feature plugins
    // (ImageDiff, IsaacModSorter, ...) never do; their game_id is just the
    // module stem, so gate any "list of games" on this, never on game_id.
    bool game_support = false;
    uint32_t abi_version = 0;
    bool loaded = false;
    bool registered = false;
    void* handle = nullptr;  // dlopen handle

    // User-facing options declared via register_settings as plain
    // key:value pairs (key = label, value = default). Source providers
    // do not use this — their settings live in the Sources tab.
    std::vector<std::pair<std::string, std::string>> settings;

    // Typed settings tab (P1.5) declared via register_settings_tab. When
    // non-empty, the host renders a dedicated Settings-dialog tab with a
    // native widget per setting (bool -> QCheckBox, int -> QSpinBox,
    // string -> QLineEdit, choice -> QComboBox) and persists edits through
    // the same per-plugin key:value store as `settings`. Keys declared here
    // stop rendering as raw rows in the Plugins-tab info pane.
    struct SettingTabEntry {
        std::string key;
        std::string type;              // "bool" | "int" | "string" | "choice"
        std::string default_value;
        std::vector<std::string> choices;  // "choice": candidate values
        std::string int_range;             // "int": "min:max", empty = default
    };
    struct SettingTab {
        std::string title;
        std::vector<SettingTabEntry> settings;
    };
    SettingTab settings_tab;
};

class PluginLoader {
public:
    PluginLoader() = default;
    ~PluginLoader();

    PluginLoader(const PluginLoader&) = delete;
    PluginLoader& operator=(const PluginLoader&) = delete;

    bool load_plugin(const std::string& path);
    bool load_directory(const std::string& dir_path);

    // Plugin basenames (e.g. "SkyrimSpecialEdition.so") skipped on load.
    void set_disabled_plugins(const std::vector<std::string>& names) {
        disabled_plugins_ = names;
    }
    [[nodiscard]] bool is_disabled(const std::string& filename) const;

    // Register a plugin that was loaded externally (e.g. by Python loader)
    void add_loaded_plugin(PluginInfo info);

    const std::vector<PluginInfo>& plugins() const { return plugins_; }
    std::vector<PluginInfo>& plugins_mutable() { return plugins_; }

    // Only the plugins that registered game support (register_identity) —
    // the ones that can back an instance. Feature/tool plugins are excluded.
    [[nodiscard]] std::vector<PluginInfo> game_plugins() const {
        std::vector<PluginInfo> games;
        for (const auto& p : plugins_)
            if (p.game_support) games.push_back(p);
        return games;
    }

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

    // Image diff provider - tool plugin for merging conflicting sprite files
    void register_image_diff(GmmImageDiffFn fn, void* user_data) {
        image_diff_.fn = fn;
        image_diff_.user_data = user_data;
    }
    [[nodiscard]] bool has_image_diff() const { return image_diff_.fn != nullptr; }
    [[nodiscard]] const ImageDiffProvider& image_diff_provider() const { return image_diff_; }

    // Run every registered diagnostics provider for game_id over db, replacing
    // each plugin's GamePlugin::messages. Call after the plugin database
    // refreshes so the Plugins tab tooltip can render them.
    void collect_diagnostics(const std::string& game_id, PluginDatabase& db);

private:
    void* dlopen_handle(const std::string& path);
    void unload_all();

    std::vector<PluginInfo> plugins_;
    std::vector<std::string> disabled_plugins_;
    StageRegistry stage_registry_;
    HookRegistry hook_registry_;
    GameCapabilities capabilities_;
    ToolRegistry tool_registry_;
    GameKnowledge knowledge_;
    ImageDiffProvider image_diff_;
};

}  // namespace engine
