#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace engine::Source {

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

// Parsed components of a modl:// URL (mod.pub / anywhere-else download
// handler - spin-off of MO2's nxmhandler). The host is a MO2 GameShortName
// ("falloutnv", "skyrimse", "other", ...); the single query param `url`
// carries a percent-encoded https direct download link.
struct ModlLink {
    std::string game_id;     // normalized MO2 GameShortName (e.g. "falloutnv")
    std::string direct_url;  // decoded https://... the link ultimately downloads
    std::string full_url;    // original modl:// URL for logging

    [[nodiscard]] bool valid() const {
        return !game_id.empty() && !direct_url.empty();
    }
};

// Routes nxm:// and modl:// URLs to the correct game instance.
// Pure C++ - no Qt dependency.
class Router {
public:
    // Parse an nxm:// URL into its components.
    // Returns a valid NxmLink if parsing succeeded.
    [[nodiscard]] static NxmLink parse(const std::string& url);

    // Parse a modl:// URL into its components. Rejects non-https direct URLs
    // and caps the decoded length to defend against runaway payloads. Returns
    // an invalid ModlLink on any failure.
    [[nodiscard]] static ModlLink parse_modl(const std::string& url);

    // Normalize a MO2 GameShortName to GMM's canonical game_id. Returns the
    // input unchanged when no alias applies.
    [[nodiscard]] static std::string normalize_modl_game_id(
        const std::string& host);

    // Match a nexus_domain to a game_id via the registered plugin's nexus_domain.
    // Returns empty string if no match.
    [[nodiscard]] static std::string match_game(
        const std::string& nexus_domain,
        const std::vector<std::pair<std::string, std::string>>& plugin_domains);
        // ^ (game_id, nexus_domain) pairs from loaded plugins
};

} // namespace engine::Source
