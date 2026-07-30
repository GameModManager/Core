#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace engine {

// Parsed components of an nxm:// URL.
struct NxmLink {
    std::string nexus_domain;  // e.g. "skyrimspecialedition", "isaac"
    int64_t mod_id = 0;        // mod ID (0 if not present)
    int64_t file_id = 0;       // file ID (0 if not present)
    std::string key;           // download key
    int64_t expire = 0;        // key expiry timestamp
    int64_t user_id = 0;       // Nexus user ID
    std::string full_url;      // original URL for logging

    [[nodiscard]] bool valid() const { return !nexus_domain.empty(); }
};

// Routes nxm:// URLs to the correct game instance.
// Pure C++ — no Qt dependency.
class NxmRouter {
public:
    // Parse an nxm:// URL into its components.
    // Returns a valid NxmLink if parsing succeeded.
    [[nodiscard]] static NxmLink parse(const std::string& url);

    // Match a nexus_domain to a game_id via the registered plugin's nexus_domain.
    // Returns empty string if no match.
    [[nodiscard]] static std::string match_game(
        const std::string& nexus_domain,
        const std::vector<std::pair<std::string, std::string>>& plugin_domains);
        // ^ (game_id, nexus_domain) pairs from loaded plugins
};

}  // namespace engine
