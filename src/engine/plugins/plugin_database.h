#pragma once

#include "engine/plugins/plugin_info.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace engine {

class GameKnowledge;
class PlatformInterface;

// Game plugin list: discovery, load order, enable state, masters, mod
// indexes, and MO2-compatible plugins.txt / loadorder.txt / lockedorder.txt
// persistence. A Qt-free port of MO2's PluginList
// (modorganizer/src/pluginlist.cpp) scoped to what a mod manager needs to
// make the game actually load mods.
class PluginDatabase {
public:
    // Discover + parse all plugins from the game's merged Data view.
    //   game_dir   - game install root (Data/ holds vanilla + game-native plugins)
    //   mods_dir   - instance mods dir
    //   meta_dir   - instance meta dir (mod priority sidecars; may be empty)
    //   disable_mechanism - sentinel filename marking a mod disabled (may be empty)
    //   game_native_plugins - comma-separated vanilla plugins (game_native_plugins hook)
    bool refresh(const std::filesystem::path& game_dir,
                 const std::filesystem::path& mods_dir,
                 const std::filesystem::path& meta_dir,
                 const std::string& disable_mechanism,
                 const std::string& game_native_plugins);

    // Read Skyrim.ccc (game root, then Data/) and mark listed content as CC
    // (force-loaded, excluded from plugins.txt). Call before sort_load_order().
    void load_creation_club(const std::filesystem::path& game_dir);

    // Parse TES4 headers for all discovered plugins (extension fallbacks
    // apply when a file can't be parsed). Called by refresh().
    void parse_headers();

    // Order plugins: game-native first (declared order), then CC (ccc order),
    // then mod plugins topologically sorted by masters with ties broken by
    // mod priority. Also flags plugins whose masters are absent.
    void sort_load_order();

    // Enable every non-force-loaded plugin. Default state for a first run
    // (no persisted profile yet) - matches the game's own behavior of loading
    // every plugin it finds in Data.
    void set_all_enabled();

    // Recompute missing_master flags for the current list.
    void set_missing_masters();

    // Assign formID prefixes (Mod Index column) after the load order is set.
    void generate_mod_indexes();

    // Toggle a plugin. Force-loaded plugins (game-native, CC) reject disable.
    // Enabling a plugin transitively enables its masters; disabling a plugin
    // that enabled plugins depend on is blocked with a message.
    // Returns false with *error set on failure.
    bool set_enabled(const std::string& name, bool enabled,
                     std::string* error = nullptr);

    // Move a plugin within the user band (below the fixed game-native + CC
    // rows). Fixed rows are rejected; out-of-range drops are clamped. Priority
    // is recomputed as the row index and mod indexes regenerated afterwards.
    // Returns false with *error set on failure.
    bool move_plugin(int from_row, int to_row, std::string* error = nullptr);

    // Load profile state (plugins.txt/loadorder.txt/lockedorder.txt) from
    // <profiles_dir>/<profile_name>/. Returns true when state was applied.
    // *repaired (optional) is set when the loaded order violated the native/CC
    // band invariant (a core plugin below user plugins) and was healed.
    bool load_profile(const std::filesystem::path& profiles_dir,
                      const std::string& profile_name,
                      bool* repaired = nullptr);

    // Persist the current state in MO2-compatible files.
    void save_profile(const std::filesystem::path& profiles_dir,
                      const std::string& profile_name) const;

    // Write the game's plugins.txt (enabled = '*', game-native + CC excluded).
    bool write_game_plugins_txt(const std::filesystem::path& path) const;

    // Write MO2-style loadorder.txt (all plugins, first line = first-loaded).
    bool write_load_order_txt(const std::filesystem::path& path) const;

    [[nodiscard]] const std::vector<GamePlugin>& plugins() const { return plugins_; }
    [[nodiscard]] const GamePlugin* find(const std::string& name) const;

    // --- Launch-time helpers ---------------------------------------------

    // Canonical resolve of the game's plugins.txt target: an explicit
    // override wins; else platform-resolved %LOCALAPPDATA%/<localappdata_folder>/Plugins.txt.
    // Returns empty when the game has no plugin support (no localappdata_folder
    // hook) or the target can't be resolved.
    static std::filesystem::path resolve_plugins_txt_target(
        const GameKnowledge& knowledge,
        const std::string& game_id,
        uint32_t steam_appid,
        const PlatformInterface* platform,
        const std::filesystem::path& override_path = {});

    // Build the plugin list from on-disk state and write plugins.txt to the
    // game's target (an instance.toml plugins_txt_path entry, or the
    // platform-resolved default). Honors a persisted profile's enable state;
    // without one, enables everything so installed mods actually load.
    // Returns false (and skips silently) for games without plugin support.
    static bool write_plugins_txt_for_launch(
        const std::filesystem::path& game_dir,
        const std::filesystem::path& instance_root,
        const std::string& game_id,
        uint32_t steam_appid,
        const GameKnowledge& knowledge,
        const PlatformInterface* platform);

    // Default profile name (matches MO2's "Default" profile).
    static constexpr const char* kDefaultProfile = "Default";

private:
    void rebuild_index();

    // Reassert the fixed band invariant: game-native plugins first (declared
    // order, then any remaining), then Creation Club (ccc order, then any
    // remaining), then everything else in its current (user/LOOT) relative
    // order. Returns true when the order was actually changed. Called after
    // profile order restore and defensively after every move, so a stale or
    // hand-edited loadorder.txt can never park a core plugin below user ones.
    bool reassert_band();

    // Plugin indices by name for lookup + ordering.
    std::map<std::string, size_t> by_name_;
    // Lowercased-key index for master lookups. Master names come from TES4
    // header MAST records, which are byte-exact; plugin names come from
    // on-disk filenames. Games run on a case-insensitive filesystem
    // (Windows), so matching must ignore case or a "skyrim.esm" header
    // reference fails against an on-disk "Skyrim.esm".
    std::map<std::string, size_t> by_name_ci_;
    std::vector<GamePlugin> plugins_;

    // Game-native plugins in the order declared by the game module.
    std::vector<std::string> native_order_;
    // CC plugins in the order listed by Skyrim.ccc.
    std::vector<std::string> ccc_order_;
};

}  // namespace engine
