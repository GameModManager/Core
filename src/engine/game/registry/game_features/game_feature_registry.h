#pragma once

// P1.2 GameFeatureRegistry — MO2's IGameFeatures analogue (PLAN.md §19.3 gap 2
// / §19.4 P1.2). Mirrors SortRegistry/diagnostics_registry: a process-wide
// singleton (Qt-free) plugins populate through register_game_feature (C ABI)
// or its pybind mirror, and the engine queries at use sites.
//
// Semantics, matching REFERENCES/modorganizer/src/game_features.{h,cpp}:
//   - register_feature(): priority + replace. Higher priority wins resolve();
//     equal priority = last registered wins. The game's own feature registers
//     at the LOWEST priority (the baseline everything else overrides).
//   - resolve(): the single highest-priority registered feature (MO2
//     gameFeature<T>() returning the front of the priority-sorted list).
//   - resolve_mod_data_checker(): MO2's CombinedModDataChecker — ALL registered
//     checkers OR together (ANY checker VALID -> VALID). The union's allow-set
//     drives the mod list's FLAG_INVALID ("No valid game data").
//   - resolve_game_plugins(): MO2's GamePlugins::gamePlugins() — the game's
//     vanilla plugin files (unmanaged top band); a registered feature replaces
//     the old game_native_plugins knowledge hook. Consumers go through the free
//     native_plugins_csv() helper below (registry-first, hook fallback).

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "engine/game/registry/game_features/game_feature.h"
#include "engine/game/registry/game_knowledge.h"

namespace engine {

struct RegisteredGameFeature {
  std::string game_id;
  std::string feature_type;
  int priority = 0;
  std::string source; // plugin path, for diagnostics/logging
  std::shared_ptr<GameFeature> feature;
};

namespace Game {
namespace Features {

class Registry {
public:
  static Registry &instance();

  // Register a feature for (game_id, feature_type). Returns false (and logs)
  // when feature or feature_type is empty. Registration order is preserved;
  // see resolve() for the priority + replace rules.
  bool register_feature(const std::string &game_id,
                        const std::string &feature_type, int priority,
                        std::shared_ptr<GameFeature> feature,
                        const std::string &source = "");

  // Highest-priority feature for (game_id, type), else nullptr. Equal
  // priority: the LAST registered wins (a later plugin registering the same
  // priority supersedes the earlier one).
  [[nodiscard]] std::shared_ptr<GameFeature>
  resolve(const std::string &game_id, const std::string &feature_type) const;

  // Combined ModDataChecker for a game: a feature whose allow-sets are the
  // union of ALL registered mod_data_checker features' folder_names() and
  // file_extensions(). nullptr when none registered for the game.
  [[nodiscard]] std::shared_ptr<const ModDataCheckerFeature>
  resolve_mod_data_checker(const std::string &game_id) const;

  // Highest-priority GamePluginsFeature (the game's vanilla plugin files,
  // MO2 GamePlugins::gamePlugins()), else nullptr.
  [[nodiscard]] std::shared_ptr<const GamePluginsFeature>
  resolve_game_plugins(const std::string &game_id) const;

  // Typed resolve for any feature class exposing a static type_key(): the
  // highest-priority registered feature of T's type, else nullptr. E.g.
  // resolve_feature<ScriptExtenderFeature>(game_id).
  //
  // Wildcard fallback: if no feature is registered for the exact game_id,
  // retry with the empty game_id. Features registered under the empty
  // game_id are global / non-game-specific (e.g. a file-format animation
  // parser that applies to every game), so they are returned when no
  // game-specific feature exists. This is what lets a generic plugin such as
  // Anm2Support serve any game without being re-registered per game.
  template <class T>
  [[nodiscard]] std::shared_ptr<const T>
  resolve_feature(const std::string &game_id) const {
    auto f = resolve(game_id, T::type_key());
    if (!f)
      f = resolve("", T::type_key()); // global / wildcard fallback
    return std::dynamic_pointer_cast<const T>(f);
  }

  // All registrations for (game_id, type) in registration order (for
  // diagnostics/tests). Empty when nothing matches.
  [[nodiscard]] std::vector<RegisteredGameFeature>
  features_for(const std::string &game_id,
               const std::string &feature_type) const;

  // Drop all registrations (Python shutdown path, test teardown).
  void clear();

private:
  Registry() = default;
  std::vector<RegisteredGameFeature> features_;
};

} // namespace Features
} // namespace Game

// Backward-compatible alias for existing consumers
using GameFeatureRegistry = Game::Features::Registry;

// The game's native (unmanaged) plugin list as comma-separated CSV — the
// format every consumer already parses (PluginDatabase::refresh,
// MainWindow preload/refresh, ModScanWorker unmanaged-row synthesis).
// Resolved registry-first: a registered "game_plugins" feature (MO2
// GamePlugins::gamePlugins()), else the game_native_plugins knowledge hook,
// else empty. Keeping the two sources behind one function is what lets a
// plugin override Skyrim's vanilla ESM band the way the data-checker can be
// overridden.
[[nodiscard]] std::string native_plugins_csv(const GameKnowledge &knowledge,
                                             const std::string &game_id);

// The registered UnmanagedModsFeature's internal mod names (MO2
// IUnmanagedMods::mods(false)), empty when none is registered. ModScanWorker
// merges these into its unmanaged-row synthesis so a plugin can declare mods
// the game manages itself (DLC/CC folders) that must show in the list.
[[nodiscard]] std::vector<std::string>
unmanaged_mods_for(const std::string &game_id);

// Register a game feature whose payload is key/value pairs — the
// register_game_feature_data ABI entry (and its pybind mirror) land here, so
// the parse logic lives once and is directly testable. This is the path for
// the seven structured-data feature types; the two array-payload types
// (mod_data_checker, game_plugins) go through GameFeatureRegistry directly.
// Keys per feature_type (see gmm_abi_v1.h register_game_feature_data):
//   mod_data_content  — "enabled" (comma-separated catalog IDs),
//                       "content:<id>" = "name|icon|filter_only" (override).
//   data_archives     — "vanilla_archives" (comma-separated archive names).
//   script_extender   — "binary", "plugin_path", "loader_name",
//                       "savegame_extension".
//   save_game_info    — "extensions" (comma-separated save extensions).
//   local_savegames   — "saves_subpath", "ini_file".
//   unmanaged_mods    — "mods" (comma-separated internal mod names).
//   bsa_invalidation  — "bsa_name", "bsa_version".
// Unknown feature_type or an empty game_id/type is logged and returns false.
[[nodiscard]] bool register_game_feature_data(
    const std::string &game_id, const std::string &feature_type, int priority,
    const std::vector<std::pair<std::string, std::string>> &kv,
    const std::string &source = "");

} // namespace engine
