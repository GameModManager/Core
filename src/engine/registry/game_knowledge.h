#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace engine {

// Simple key-value knowledge store per game.
// Plugins register game-specific data (file extensions, patterns, etc.)
// that the engine queries when managing that game's mods.
//
// Keys are namespaced: "conflict_extensions", "ignored_files",
// "workshop_id_pattern", "disable_mechanism", "auto_sort_groups", etc.
// Values are strings (comma-separated lists, JSON, regex - keyed by convention).
class GameKnowledge {
public:
    void set(const std::string& game_id, const std::string& key, const std::string& value);

    [[nodiscard]] std::string get(const std::string& game_id,
                                   const std::string& key,
                                   const std::string& fallback = "") const;

    [[nodiscard]] bool has(const std::string& game_id, const std::string& key) const;

    [[nodiscard]] std::vector<std::string> keys_for(const std::string& game_id) const;

    [[nodiscard]] std::vector<std::string> registered_games() const;

    void clear();

private:
    // game_id → key → value
    std::unordered_map<std::string,
        std::unordered_map<std::string, std::string>> data_;
};

}  // namespace engine
