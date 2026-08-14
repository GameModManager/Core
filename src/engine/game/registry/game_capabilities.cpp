#include "engine/game/registry/game_capabilities.h"
#include "engine/core/log/logger.h"

#include <algorithm>
#include <map>
#include <queue>
#include <set>

namespace engine {

void GameCapabilities::register_capability(const CapabilityInfo& info) {
    caps_[info.game_id][info.capability] = info;
    Logger::instance().debug("Capability registered: " + info.game_id + "." + info.capability +
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

std::vector<CapabilityInfo> GameCapabilities::sorted_capabilities_for(
    const std::string& game_id) const {
    auto game_it = caps_.find(game_id);
    if (game_it == caps_.end()) return {};

    // Collect all nodes - include "data" as an anchor
    std::set<std::string> all_nodes;
    all_nodes.insert("data");
    for (const auto& [name, _] : game_it->second) {
        all_nodes.insert(name);
    }

    // Build adjacency list and in-degree map
    std::map<std::string, std::vector<std::string>> graph;
    std::map<std::string, int> in_degree;
    for (const auto& node : all_nodes) {
        in_degree[node] = 0;
    }

    for (const auto& [name, info] : game_it->second) {
        if (!info.insert_before.empty()) {
            // info should appear before insert_before
            graph[name].push_back(info.insert_before);
            in_degree[info.insert_before]++;
        }
        if (!info.insert_after.empty()) {
            // info should appear after insert_after
            graph[info.insert_after].push_back(name);
            in_degree[name]++;
        }
    }

    // Kahn's algorithm for topological sort
    std::queue<std::string> q;
    for (const auto& [node, deg] : in_degree) {
        if (deg == 0) q.push(node);
    }

    std::vector<std::string> sorted;
    while (!q.empty()) {
        auto node = q.front(); q.pop();
        sorted.push_back(node);
        for (const auto& neighbor : graph[node]) {
            if (--in_degree[neighbor] == 0) q.push(neighbor);
        }
    }

    // Resolve any remaining nodes (cycles - fall back to arbitrary order)
    for (const auto& node : all_nodes) {
        if (std::find(sorted.begin(), sorted.end(), node) == sorted.end()) {
            sorted.push_back(node);
        }
    }

    // Build result from sorted order, merging explicit "data" with built-in
    std::vector<CapabilityInfo> result;
    for (const auto& name : sorted) {
        if (name == "data") {
            // Insert the built-in Data tab at this position
            CapabilityInfo data_info;
            data_info.capability = "data";
            data_info.display_name = "Data";
            data_info.game_id = game_id;
            // Copy ordering from explicit registration if present, otherwise no constraints
            auto it = game_it->second.find("data");
            if (it != game_it->second.end()) {
                data_info.insert_before = it->second.insert_before;
                data_info.insert_after = it->second.insert_after;
            }
            result.push_back(data_info);
        } else {
            auto it = game_it->second.find(name);
            if (it != game_it->second.end()) {
                result.push_back(it->second);
            }
        }
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
