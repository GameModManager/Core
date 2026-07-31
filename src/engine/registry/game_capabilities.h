#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine {

// A game module registers which capabilities it supports.
// The UI queries these to decide which tabs to show and how to populate them.
// "Data" is always shown - it's the universal view of what the game sees on disk.

struct CapabilityInfo {
    std::string game_id;
    std::string capability;   // "plugins", "archives", "saves", "downloads", "conflicts"
    std::string display_name; // e.g. "Plugins", "Archives"
    std::string data_path;    // where the game stores these (e.g. "Data/", "Documents/My Games/...")
    std::string description;  // human-readable hint for UI tooltip/status bar

    // For "downloads" capability specifically
    std::string protocol_handler;  // e.g. "nxm", "steamworkshop"
    std::string website_domain;    // e.g. "nexusmods.com"
    std::vector<std::string> supported_platforms;  // e.g. {"nexus", "workshop", "moddb"}

    // Tab ordering - reference other capability_ids or "data"
    std::string insert_before;
    std::string insert_after;
};

class GameCapabilities {
public:
    // Register a capability for a game
    void register_capability(const CapabilityInfo& info);

    // Check if a game supports a capability
    [[nodiscard]] bool has_capability(const std::string& game_id,
                                      const std::string& capability) const;

    // Get all capabilities for a game
    [[nodiscard]] std::vector<CapabilityInfo> capabilities_for(
        const std::string& game_id) const;

    // Get capabilities sorted for tab display (respects insert_before/insert_after)
    [[nodiscard]] std::vector<CapabilityInfo> sorted_capabilities_for(
        const std::string& game_id) const;

    // Get a specific capability
    [[nodiscard]] const CapabilityInfo* get_capability(
        const std::string& game_id,
        const std::string& capability) const;

    // Get all registered game IDs
    [[nodiscard]] std::vector<std::string> registered_games() const;

    void clear();

    // Convenience: the set of tabs that should be shown for a game
    // Always includes "data". Returns capability display_names.
    [[nodiscard]] std::vector<std::string> visible_tabs_for(
        const std::string& game_id) const;

private:
    // game_id -> capability_name -> info
    std::unordered_map<std::string,
        std::unordered_map<std::string, CapabilityInfo>> caps_;
};

}  // namespace engine
