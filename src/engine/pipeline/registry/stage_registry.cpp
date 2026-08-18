#include "engine/pipeline/registry/stage_registry.h"
#include "engine/core/log/logger.h"

#include <algorithm>

namespace engine {

void StageRegistry::register_claim(const std::string& game_id,
                                    const std::string& stage_name,
                                    StageFn handler,
                                    int priority,
                                    const std::string& plugin_id) {
    StageClaim claim;
    claim.game_id = game_id;
    claim.stage_name = stage_name;
    claim.handler = std::move(handler);
    claim.priority = priority;
    claim.plugin_id = plugin_id;

    claims_.push_back(std::move(claim));

    Logger::instance().debug("Stage claim registered: " +
        (game_id.empty() ? std::string("*") : game_id) + "." + stage_name +
        " by " + (plugin_id.empty() ? "unknown" : plugin_id) +
        " (priority " + std::to_string(priority) + ")");
}

StageFn StageRegistry::get_handler(const std::string& game_id,
                                    const std::string& stage_name) const {
    StageFn best_handler;
    int best_priority = INT32_MIN;
    bool best_is_wildcard = false;

    for (const auto& claim : claims_) {
        if (claim.stage_name != stage_name) continue;

        // A claim matches when its game_id equals the requested one, or when
        // it is a wildcard (empty game_id) that matches any game.
        const bool claim_is_wildcard = claim.game_id.empty();
        if (!claim_is_wildcard && claim.game_id != game_id) continue;

        if (!best_handler) {
            best_handler = claim.handler;
            best_priority = claim.priority;
            best_is_wildcard = claim_is_wildcard;
            continue;
        }

        if (claim.priority > best_priority) {
            // Higher priority always wins.
            best_handler = claim.handler;
            best_priority = claim.priority;
            best_is_wildcard = claim_is_wildcard;
        } else if (claim.priority == best_priority) {
            if (best_is_wildcard && !claim_is_wildcard) {
                // A game-specific claim beats a wildcard at equal priority.
                best_handler = claim.handler;
                best_priority = claim.priority;
                best_is_wildcard = false;
            } else if (claim_is_wildcard && !best_is_wildcard) {
                // The wildcard loses to the game-specific claim; not a conflict.
                continue;
            } else {
                // Same kind of claim at the same priority — genuine conflict.
                Logger::instance().warn("Conflicting stage claims for " +
                    game_id + "." + stage_name + " - same priority, keeping first");
            }
        }
    }

    return best_handler;
}

bool StageRegistry::has_claim(const std::string& game_id,
                               const std::string& stage_name) const {
    for (const auto& claim : claims_) {
        if (claim.stage_name != stage_name) continue;
        if (claim.game_id.empty() || claim.game_id == game_id) {
            return true;
        }
    }
    return false;
}

void StageRegistry::clear() {
    claims_.clear();
}

}  // namespace engine
