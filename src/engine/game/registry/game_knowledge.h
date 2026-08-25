#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine {

// Simple key-value knowledge store per game.
// Plugins register game-specific data (file extensions, patterns, etc.)
// that the engine queries when managing that game's mods.
//
// Keys are namespaced: "conflict_extensions", "ignored_files",
// "workshop_id_pattern", "disable_mechanism", "auto_sort_groups", etc.
// Values are strings (comma-separated lists, JSON, regex - keyed by convention).
class GameKnowledge {
public:
    void set(const std::string& game_id, const std::string& key, const std::string& value);

    [[nodiscard]] std::string get(const std::string& game_id,
                                   const std::string& key,
                                   const std::string& fallback = "") const;

    [[nodiscard]] bool has(const std::string& game_id, const std::string& key) const;

    [[nodiscard]] std::vector<std::string> keys_for(const std::string& game_id) const;

    [[nodiscard]] std::vector<std::string> registered_games() const;

    void clear();

private:
    // game_id → key → value
    std::unordered_map<std::string,
        std::unordered_map<std::string, std::string>> data_;
};

// Default disable sentinel filename used when a game plugin declares no
// "disable_mechanism" hook. The engine writes this file into a mod folder to
// mark it disabled, and every consumer (deploy, plugin DB, mod scanner) treats
// it as authoritative — so "disabled" never silently becomes a no-op for games
// that ship no game-native marker (Skyrim) the way Isaac's "disable.it" does.
inline constexpr const char* kDefaultDisableMechanism = ".gmmdisabled";

// Sentinel filename marking a mod disabled for the given game. Falls back to
// kDefaultDisableMechanism when the game plugin declares nothing — a game's
// declared mechanism (e.g. Isaac's "disable.it") always takes precedence.
[[nodiscard]] std::string disable_mechanism_for(const GameKnowledge& knowledge,
                                                const std::string& game_id);

// True when the game plugin declares delayed_disable=true: the engine must
// defer disable-sentinel disk writes until the Run/deploy phase. Games that
// deploy directly into the game dir (e.g. Isaac's Direct mode) declare it so
// toggling a mod never touches the game dir while the game may be reading it;
// the sentinel is reconciled from the profile at launch instead. All other
// games (Skyrim, ...) leave it undeclared -> false -> immediate disk writes
// (current behavior).
[[nodiscard]] bool delayed_disable_for(const GameKnowledge& knowledge,
                                       const std::string& game_id);

// Deploy strategy names for the per-game "deploy_strategy" knowledge key.
// The default is Symlink (direct symlinks into game_dir); a game opts out of
// that by setting the key to kDeployStrategyOverlayFs, which deploys into a
// staging dir and overlays it onto the game at launch (Linux only).
// kDeployStrategyDirect is the lifecycle-object form of the symlink default:
// it deploys straight into game_dir through DirectDeployStrategy (the same
// on-disk result, but with deploy_all/undeploy/sync as first-class methods).
inline constexpr const char* kDefaultDeployStrategy = "symlink";
inline constexpr const char* kDeployStrategyOverlayFs = "overlayfs";
inline constexpr const char* kDeployStrategyDirect = "direct";

// Deploy strategy declared for the given game. Falls back to
// kDefaultDeployStrategy when the game plugin declares nothing.
[[nodiscard]] std::string deploy_strategy_for(const GameKnowledge& knowledge,
                                              const std::string& game_id);

// Plugin-declared absolute game-mods directory ("game_mods_dir" hook, e.g.
// Isaac on macOS reads mods from ~/Library/Application Support/...), with a
// leading ~ expanded against $HOME. Empty when the plugin declares nothing.
[[nodiscard]] std::string plugin_game_mods_dir(const GameKnowledge& knowledge,
                                               const std::string& game_id);

// The game's native mods directory, resolved once for every consumer:
//   1. override_dir (the instance.toml "game_mods_dir") when non-empty,
//   2. the plugin-declared "game_mods_dir" hook (~-expanded),
//   3. game_dir / "mods_subpath" when the plugin declares one,
//   4. game_dir.
// Deploy is the exception: it consumes only steps 1-2 (via
// plugin_game_mods_dir) because folding mods_subpath into the deploy root
// would misplace root-override mods that must land in the game root.
[[nodiscard]] std::filesystem::path resolve_game_mods_dir(
    const std::string& game_id,
    const std::filesystem::path& game_dir,
    const GameKnowledge& knowledge,
    const std::string& override_dir = "");

}  // namespace engine
