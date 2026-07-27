#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine {

// An external tool registered by a game module.
// §7: two categories — advisory (output feeds pipeline) and workshop (user launches directly).
enum class ToolKind {
    Advisory,   // LOOT: run it, parse output, hand to OrderEncodingHook
    Workshop,   // BodySlide: user runs it, engine captures generated files
};

struct ExternalTool {
    std::string tool_id;        // "loot", "bodyslide", "xedit"
    std::string game_id;        // which game this tool is for
    ToolKind kind = ToolKind::Advisory;
    std::string display_name;   // "LOOT", "BodySlide"
    std::string description;    // human-readable

    // Detection — where to find the executable
    std::string executable_name;// "LOOT.exe", "BodySlidex64.exe"
    std::vector<std::string> search_paths; // known install locations
    std::string registry_key;   // Windows registry key (Phase 3)

    // Invocation template
    std::string invoke_args;    // command-line args, e.g. "--game {game_id} --out {output}"
    std::string working_dir;    // working directory template

    // Callback — invoked by the engine when the tool needs to run
    std::function<void(void*)> invoke_fn;
    void* invoke_user_data = nullptr;
};

class ToolRegistry {
public:
    void register_tool(const ExternalTool& tool);

    // All tools for a game
    [[nodiscard]] std::vector<ExternalTool> tools_for_game(const std::string& game_id) const;

    // Specific tool lookup
    [[nodiscard]] const ExternalTool* get_tool(const std::string& game_id,
                                                const std::string& tool_id) const;

    // Filtered by kind
    [[nodiscard]] std::vector<ExternalTool> advisory_tools_for(const std::string& game_id) const;
    [[nodiscard]] std::vector<ExternalTool> workshop_tools_for(const std::string& game_id) const;

    // All registered game IDs
    [[nodiscard]] std::vector<std::string> registered_games() const;

    void clear();

private:
    // game_id -> tool_id -> tool
    std::unordered_map<std::string,
        std::unordered_map<std::string, ExternalTool>> tools_;
};

}  // namespace engine
