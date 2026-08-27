#pragma once

#include "engine/game/registry/game_knowledge.h"

#include <filesystem>
#include <string>

namespace engine {

// Knowledge key game plugins use to declare their masterlist URL
// (register_hook).
inline constexpr const char *kMasterlistUrlKey = "masterlist_url";

// Masterlist cache filename
inline constexpr const char *kMasterlistFilename = "masterlist.yaml";

// Cache directory for masterlists: <data_dir>/masterlists
[[nodiscard]] std::filesystem::path masterlist_cache_dir();

// Cached masterlist path for a game.
[[nodiscard]] std::filesystem::path
cached_masterlist_path(const std::string &game_id);

// Declared masterlist URL for a game; empty when the plugin declares none.
[[nodiscard]] std::string masterlist_url_for(const GameKnowledge &knowledge,
                                             const std::string &game_id);

// Download masterlist from url into the cache if not already present.
// Returns true when a valid cached file exists on return; error is populated
// only on failure.
[[nodiscard]] bool ensure_masterlist_cached(const std::string &game_id,
                                            const std::string &url,
                                            std::string &error);

// Read the cached masterlist content. Returns empty string if not cached.
[[nodiscard]] std::string read_cached_masterlist(const std::string &game_id);

} // namespace engine
