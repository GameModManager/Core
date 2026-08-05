#pragma once

#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace engine {

// Deploy-space classification for a conflict-registry path.
//
// A mod flagged [General] rootOverride deploys its folder into the game root
// instead of the data dir (deploy_utils.cpp honors this on every launch). The
// conflict registry keys paths mod-relative, so a registry entry must be
// mapped to the view that shows it, and re-written for display:
//   - no root-override owner        -> Data space, path as-is
//   - root-override owner + a leading <deploy_prefix>/ segment
//                                    -> Data space, segment stripped
//     (the mod's "Data/..." content still lands in the game's data dir)
//   - root-override owner, no such segment -> Root space, path as-is
//
// deploy_prefix is the game-relative subpath mods deploy into when NOT
// root-flagged (Skyrim: "Data"; Isaac: "mods") - i.e. the data-dir name the
// staging tree uses. ASCII case-insensitive on the first segment.
enum class DeploySpace { Data, Root };

struct ClassifiedPath {
    DeploySpace space = DeploySpace::Data;
    std::string display_path;  // path relative to the space's view root
};

// owners: (mod_id, priority) providers of rel_path, winner-first order is not
// required (only membership in root_override_mods is consulted).
ClassifiedPath classify_registry_path(
    const std::string& rel_path,
    const std::vector<std::pair<std::string, int>>& owners,
    const std::unordered_set<std::string>& root_override_mods,
    const std::string& deploy_prefix);

}  // namespace engine
