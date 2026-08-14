#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace engine {

// A single download source for a game (Nexus Mods, LoversLab, etc.)
struct GameSource {
    std::string source_id;      // e.g. "nexus", "loverslab", "moddb"
    std::string website_url;    // e.g. "nexusmods.com", "loverslab.com"
    std::string nexus_domain;   // nxm:// domain for this source (empty if not nxm-based)
};

// Which games GameModManager is currently managing, and which sources
// are registered for each. Stored as JSON in the data directory.
struct ManagedGameEntry {
    std::string game_id;
    std::vector<GameSource> sources;
};

class ManagedGames {
public:
    explicit ManagedGames(const std::filesystem::path& json_path);

    void load();
    bool save() const;

    [[nodiscard]] bool is_managed(const std::string& game_id) const;

    // Check if a specific source is registered for a game
    [[nodiscard]] bool has_source(const std::string& game_id,
                                  const std::string& source_id) const;

    // Add a source to a game (creates the game entry if needed)
    void add_source(const std::string& game_id, const GameSource& source);

    // Remove a source from a game
    void remove_source(const std::string& game_id, const std::string& source_id);

    // Find which game_id owns a given nexus_domain
    [[nodiscard]] std::string game_id_for_domain(const std::string& nexus_domain) const;

    [[nodiscard]] const std::vector<ManagedGameEntry>& entries() const { return entries_; }

private:
    std::filesystem::path json_path_;
    std::vector<ManagedGameEntry> entries_;
};

}  // namespace engine
