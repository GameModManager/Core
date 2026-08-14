#pragma once

#include "engine/game/registry/game_knowledge.h"

#include <filesystem>
#include <string>

namespace engine {

// Knowledge key game plugins use to declare their icon URL (register_hook).
// The engine downloads the URL into its global icon cache on first use and
// the UI enforces the target size when rendering.
inline constexpr const char* kIconUrlKey = "game_icon_url";

// Global game-icon cache directory: <data root>/cache/icons
// (data root = parent of default_instances_dir(), e.g.
// ~/.local/share/GameModManager/cache/icons).
[[nodiscard]] std::filesystem::path icon_cache_dir();

// Full path of a game's cached icon (keyed by game_id, .png).
[[nodiscard]] std::filesystem::path cached_icon_path(const std::string& game_id);

// Declared icon URL for a game; empty when the plugin declares none.
[[nodiscard]] std::string icon_url_for(const GameKnowledge& knowledge,
                                       const std::string& game_id);

// Download url into the cache if not already present. Qt-free and worker-thread
// safe (network + filesystem only). Returns true when a valid cached file
// exists on return; error is populated only on failure.
[[nodiscard]] bool ensure_icon_cached(const std::string& game_id,
                                      const std::string& url,
                                      std::string& error);

}  // namespace engine
