#include "engine/game/registry/game_knowledge.h"

#include <cstdlib>

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


std::string creation_club_file_for(const GameKnowledge& knowledge,
                                   const std::string& game_id) {
    return knowledge.get(game_id, "creation_club_file", "skyrim.ccc");
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

std::string plugin_game_mods_dir(const GameKnowledge& knowledge,
                                 const std::string& game_id) {
    std::string dir = knowledge.get(game_id, "game_mods_dir", "");
    // Expand a leading ~ against $HOME at resolution time (the plugin only
    // declares the literal path; HOME may differ between registration and use).
    if (!dir.empty() && dir.front() == '~') {
        if (const char* home = std::getenv("HOME"))
            dir = std::string(home) + dir.substr(1);
    }
    return dir;
}

std::filesystem::path resolve_game_mods_dir(
    const std::string& game_id,
    const std::filesystem::path& game_dir,
    const GameKnowledge& knowledge,
    const std::string& override_dir) {
    // 1. Per-instance user override (instance.toml "game_mods_dir") wins.
    if (!override_dir.empty()) return std::filesystem::path(override_dir);
    // 2. Plugin-declared absolute dir (e.g. Isaac on macOS).
    const std::string declared = plugin_game_mods_dir(knowledge, game_id);
    if (!declared.empty()) return std::filesystem::path(declared);
    // 3./4. Subpath under the game install, else the game dir itself.
    const std::string subpath = knowledge.get(game_id, "mods_subpath", "");
    if (subpath.empty()) return game_dir;
    return game_dir / subpath;
}

}  // namespace engine
