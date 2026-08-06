#include "engine/registry/game_features/game_feature_registry.h"

#include "engine/log/logger.h"

#include <utility>

namespace engine {

namespace {
constexpr const char* kModDataCheckerType = "mod_data_checker";
}  // namespace

GameFeatureRegistry& GameFeatureRegistry::instance() {
    static GameFeatureRegistry registry;
    return registry;
}

bool GameFeatureRegistry::register_feature(
    const std::string& game_id,
    const std::string& feature_type,
    int priority,
    std::shared_ptr<GameFeature> feature,
    const std::string& source) {
    if (feature_type.empty() || !feature) {
        Logger::instance().warn(
            "GameFeatureRegistry: refused registration with empty type/null feature");
        return false;
    }
    RegisteredGameFeature entry;
    entry.game_id = game_id;
    entry.feature_type = feature_type;
    entry.priority = priority;
    entry.source = source;
    entry.feature = std::move(feature);
    features_.push_back(std::move(entry));
    return true;
}

std::shared_ptr<GameFeature> GameFeatureRegistry::resolve(
    const std::string& game_id,
    const std::string& feature_type) const {
    std::shared_ptr<GameFeature> best;
    int best_priority = 0;
    bool have_best = false;
    // Iterate in registration order; strictly higher priority replaces, equal
    // priority keeps the LAST registration (later plugin supersedes earlier).
    for (const auto& entry : features_) {
        if (entry.game_id != game_id || entry.feature_type != feature_type) continue;
        if (!have_best || entry.priority >= best_priority) {
            best = entry.feature;
            best_priority = entry.priority;
            have_best = true;
        }
    }
    return best;
}

std::shared_ptr<const ModDataCheckerFeature> GameFeatureRegistry::resolve_mod_data_checker(
    const std::string& game_id) const {
    std::vector<std::string> folders;
    std::vector<std::string> extensions;
    bool any = false;
    for (const auto& entry : features_) {
        if (entry.game_id != game_id || entry.feature_type != kModDataCheckerType) continue;
        auto* checker = dynamic_cast<ModDataCheckerFeature*>(entry.feature.get());
        if (!checker) continue;
        any = true;
        for (const auto& d : checker->folder_names()) folders.push_back(d);
        for (const auto& e : checker->file_extensions()) extensions.push_back(e);
    }
    if (!any) return nullptr;
    return std::make_shared<const ModDataCheckerFeature>(std::move(folders),
                                                         std::move(extensions));
}

std::vector<RegisteredGameFeature> GameFeatureRegistry::features_for(
    const std::string& game_id,
    const std::string& feature_type) const {
    std::vector<RegisteredGameFeature> out;
    for (const auto& entry : features_) {
        if (entry.game_id != game_id || entry.feature_type != feature_type) continue;
        out.push_back(entry);
    }
    return out;
}

void GameFeatureRegistry::clear() {
    features_.clear();
}

}  // namespace engine
