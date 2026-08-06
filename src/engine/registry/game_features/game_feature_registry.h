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

#include <memory>
#include <string>
#include <vector>

#include "engine/registry/game_features/game_feature.h"

namespace engine {

struct RegisteredGameFeature {
    std::string game_id;
    std::string feature_type;
    int priority = 0;
    std::string source;  // plugin path, for diagnostics/logging
    std::shared_ptr<GameFeature> feature;
};

class GameFeatureRegistry {
public:
    static GameFeatureRegistry& instance();

    // Register a feature for (game_id, feature_type). Returns false (and logs)
    // when feature or feature_type is empty. Registration order is preserved;
    // see resolve() for the priority + replace rules.
    bool register_feature(const std::string& game_id,
                          const std::string& feature_type,
                          int priority,
                          std::shared_ptr<GameFeature> feature,
                          const std::string& source = "");

    // Highest-priority feature for (game_id, type), else nullptr. Equal
    // priority: the LAST registered wins (a later plugin registering the same
    // priority supersedes the earlier one).
    [[nodiscard]] std::shared_ptr<GameFeature> resolve(
        const std::string& game_id,
        const std::string& feature_type) const;

    // Combined ModDataChecker for a game: a feature whose allow-sets are the
    // union of ALL registered mod_data_checker features' folder_names() and
    // file_extensions(). nullptr when none registered for the game.
    [[nodiscard]] std::shared_ptr<const ModDataCheckerFeature> resolve_mod_data_checker(
        const std::string& game_id) const;

    // All registrations for (game_id, type) in registration order (for
    // diagnostics/tests). Empty when nothing matches.
    [[nodiscard]] std::vector<RegisteredGameFeature> features_for(
        const std::string& game_id,
        const std::string& feature_type) const;

    // Drop all registrations (Python shutdown path, test teardown).
    void clear();

private:
    GameFeatureRegistry() = default;
    std::vector<RegisteredGameFeature> features_;
};

}  // namespace engine
