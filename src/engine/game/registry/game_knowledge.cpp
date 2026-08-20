#include "engine/game/registry/game_knowledge.h"

namespace engine {

void GameKnowledge::set(const std::string& game_id,
                         const std::string& key,
                         const std::string& value) {
    data_[game_id][key] = value;
}

std::string GameKnowledge::get(const std::string& game_id,
                                const std::string& key,
                                const std::string& fallback) const {
    auto game_it = data_.find(game_id);
    if (game_it == data_.end()) return fallback;

    auto key_it = game_it->second.find(key);
    if (key_it == game_it->second.end()) return fallback;

    return key_it->second;
}

bool GameKnowledge::has(const std::string& game_id, const std::string& key) const {
    auto game_it = data_.find(game_id);
    if (game_it == data_.end()) return false;
    return game_it->second.count(key) > 0;
}

std::vector<std::string> GameKnowledge::keys_for(const std::string& game_id) const {
    auto game_it = data_.find(game_id);
    if (game_it == data_.end()) return {};

    std::vector<std::string> result;
    result.reserve(game_it->second.size());
    for (const auto& [key, _] : game_it->second) {
        result.push_back(key);
    }
    return result;
}

std::vector<std::string> GameKnowledge::registered_games() const {
    std::vector<std::string> result;
    result.reserve(data_.size());
    for (const auto& [game_id, _] : data_) {
        result.push_back(game_id);
    }
    return result;
}

void GameKnowledge::clear() {
    data_.clear();
}

std::string disable_mechanism_for(const GameKnowledge& knowledge,
                                  const std::string& game_id) {
    const std::string declared = knowledge.get(game_id, "disable_mechanism", "");
    if (!declared.empty()) return declared;
    return kDefaultDisableMechanism;
}

std::string deploy_strategy_for(const GameKnowledge& knowledge,
                                const std::string& game_id) {
    const std::string declared = knowledge.get(game_id, "deploy_strategy", "");
    if (!declared.empty()) return declared;
    return kDefaultDeployStrategy;
}

bool delayed_disable_for(const GameKnowledge& knowledge,
                         const std::string& game_id) {
    return knowledge.get(game_id, "delayed_disable", "") == "true";
}

}  // namespace engine
