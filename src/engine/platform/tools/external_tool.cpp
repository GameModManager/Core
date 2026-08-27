#include "engine/platform/tools/external_tool.h"

namespace engine {

void ToolRegistry::register_tool(const ExternalTool& tool) {
    tools_[tool.game_id][tool.tool_id] = tool;
}

std::vector<ExternalTool> ToolRegistry::tools_for_game(const std::string& game_id) const {
    std::vector<ExternalTool> result;

    // Always include global tools (registered under empty game_id)
    auto global_it = tools_.find("");
    if (global_it != tools_.end()) {
        result.reserve(global_it->second.size());
        for (const auto& [id, tool] : global_it->second) {
            result.push_back(tool);
        }
    }

    // Add game-specific tools
    auto it = tools_.find(game_id);
    if (it == tools_.end()) return result;

    for (const auto& [id, tool] : it->second) {
        result.push_back(tool);
    }
    return result;
}

const ExternalTool* ToolRegistry::get_tool(const std::string& game_id,
                                            const std::string& tool_id) const {
    auto game_it = tools_.find(game_id);
    if (game_it == tools_.end()) return nullptr;

    auto tool_it = game_it->second.find(tool_id);
    if (tool_it == game_it->second.end()) return nullptr;

    return &tool_it->second;
}

std::vector<ExternalTool> ToolRegistry::advisory_tools_for(const std::string& game_id) const {
    auto all = tools_for_game(game_id);
    std::vector<ExternalTool> result;
    for (const auto& t : all) {
        if (t.kind == ToolKind::Advisory) result.push_back(t);
    }
    return result;
}

std::vector<ExternalTool> ToolRegistry::workshop_tools_for(const std::string& game_id) const {
    auto all = tools_for_game(game_id);
    std::vector<ExternalTool> result;
    for (const auto& t : all) {
        if (t.kind == ToolKind::Workshop) result.push_back(t);
    }
    return result;
}

std::vector<std::string> ToolRegistry::registered_games() const {
    std::vector<std::string> result;
    result.reserve(tools_.size());
    for (const auto& [game_id, _] : tools_) {
        result.push_back(game_id);
    }
    return result;
}

void ToolRegistry::clear() {
    tools_.clear();
}

}  // namespace engine
