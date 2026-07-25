#include "engine/registry/game_capabilities.h"
#include "engine/log/logger.h"

namespace engine {

void GameCapabilities::register_capability(const CapabilityInfo& info) {
    caps_[info.game_id][info.capability] = info;
    Logger::instance().info("Capability registered: " + info.game_id + "." + info.capability +
        " (" + info.display_name + ")");
}

bool GameCapabilities::has_capability(const std::string& game_id,
                                       const std::string& capability) const {
    auto game_it = caps_.find(game_id);
    if (game_it == caps_.end()) return false;
    return game_it->second.count(capability) > 0;
}

std::vector<CapabilityInfo> GameCapabilities::capabilities_for(
    const std::string& game_id) const {
    auto game_it = caps_.find(game_id);
    if (game_it == caps_.end()) return {};

    std::vector<CapabilityInfo> result;
    for (const auto& [name, info] : game_it->second) {
        result.push_back(info);
    }
    return result;
}

const CapabilityInfo* GameCapabilities::get_capability(
    const std::string& game_id,
    const std::string& capability) const {
    auto game_it = caps_.find(game_id);
    if (game_it == caps_.end()) return nullptr;

    auto cap_it = game_it->second.find(capability);
    if (cap_it == game_it->second.end()) return nullptr;

    return &cap_it->second;
}

std::vector<std::string> GameCapabilities::registered_games() const {
    std::vector<std::string> result;
    for (const auto& [game_id, _] : caps_) {
        result.push_back(game_id);
    }
    return result;
}

std::vector<std::string> GameCapabilities::visible_tabs_for(
    const std::string& game_id) const {
    // "data" is always shown
    std::vector<std::string> tabs = {"Data"};

    auto game_it = caps_.find(game_id);
    if (game_it != caps_.end()) {
        for (const auto& [name, info] : game_it->second) {
            tabs.push_back(info.display_name);
        }
    }

    return tabs;
}

void GameCapabilities::clear() {
    caps_.clear();
}

}  // namespace engine
