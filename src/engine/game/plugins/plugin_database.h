#pragma once

#include "engine/game/plugins/plugin_file.h"
#include "engine/game/plugins/plugin_info.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace engine {

class GameKnowledge;
class PlatformInterface;

class PluginDatabase {
public:
    // Discover + parse all plugins from the game's merged Data view.
    //   game_dir   - game install root (Data/ holds vanilla + game-native plugins)
    //   mods_dir   - instance mods dir
    //   meta_dir   - instance meta dir (mod priority sidecars; may be empty)
    //   disable_mechanism - sentinel filename marking a mod disabled (may be empty)
    //   game_native_plugins - comma-separated vanilla plugins (resolved via
    //   engine::native_plugins_csv(): a registered "game_plugins" game feature,
    //   else the game_native_plugins knowledge hook)
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
    // Locked plugins are rejected both as the source (they never move) and as
    // the destination (a drop there would displace them).
    // Returns false with *error set on failure.
    bool move_plugin(int from_row, int to_row, std::string* error = nullptr);

    // Pin/unpin a plugin at its current position (MO2 lock load order). A
    // locked plugin can never move again: move_plugin rejects it and any
    // auto-sort (sort_load_order, LOOT) re-places it at its locked priority.
    // Force-loaded rows (game-native, CC) cannot be locked.
    // Returns false with *error set on failure.
    bool set_locked(const std::string& name, bool lock, std::string* error = nullptr);
    [[nodiscard]] bool is_locked(const std::string& name) const;

    // Reorder the user plugin band to the given load order (e.g. LOOT's sorted
    // output). Game-native and Creation Club rows keep their fixed band; locked
    // plugins are re-inserted at their pinned priorities; mod indexes are
    // regenerated. Every name in `order` must resolve to a known plugin
    // (case-insensitively) or the call fails with *error set and nothing changed.
    bool apply_load_order(const std::vector<std::string>& order,
                          std::string* error = nullptr);

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

    // Mutable access for engine-side consumers that attach per-plugin data
    // (e.g. DiagnosticsRegistry populating GamePlugin::messages after refresh).
    std::vector<GamePlugin>& plugins_mutable() { return plugins_; }

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

    // Re-insert every locked plugin at its locked priority (ascending order),
    // so auto-sorts can never move a pinned plugin. Non-locked plugins are
    // pushed down to fill the gaps. Mirrors MO2's PluginList::refreshLoadOrder.
    void apply_locked_order();

    // Plugin indices by name for lookup + ordering.
    std::map<std::string, size_t> by_name_;
    // Lowercased-key index for master lookups. Master names come from TES4
    // header MAST records, which are byte-exact; plugin names come from
    // on-disk filenames. Games run on a case-insensitive filesystem
    // (Windows), so matching must ignore case or a "skyrim.esm" header
    // reference fails against an on-disk "Skyrim.esm".
    std::map<std::string, size_t> by_name_ci_;
    std::vector<GamePlugin> plugins_;

    // Locked plugin name -> priority (MO2 lockedorder.txt state). Names of
    // plugins not currently present are kept so the pin survives uninstall.
    std::map<std::string, int> locked_order_;

    // Game-native plugins in the order declared by the game module.
    std::vector<std::string> native_order_;
    // CC plugins in the order listed by Skyrim.ccc.
    std::vector<std::string> ccc_order_;
};

}  // namespace engine
