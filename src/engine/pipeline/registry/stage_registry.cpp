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

    Logger::instance().debug("Stage claim registered: " + game_id + "." + stage_name +
        " by " + (plugin_id.empty() ? "unknown" : plugin_id) +
        " (priority " + std::to_string(priority) + ")");
}

StageFn StageRegistry::get_handler(const std::string& game_id,
                                    const std::string& stage_name) const {
    StageFn best_exact_handler;
    int best_exact_priority = INT32_MIN;
    StageFn best_wildcard_handler;
    int best_wildcard_priority = INT32_MIN;

    for (const auto& claim : claims_) {
        if (claim.stage_name != stage_name) continue;

        if (claim.game_id.empty()) {
            // Wildcard claim — applies to all games
            if (claim.priority > best_wildcard_priority) {
                best_wildcard_priority = claim.priority;
                best_wildcard_handler = claim.handler;
            } else if (claim.priority == best_wildcard_priority) {
                Logger::instance().warn("Conflicting wildcard stage claims for " +
                    stage_name + " - same priority, keeping first");
            }
        } else if (claim.game_id == game_id) {
            // Exact-match claim — game-specific
            if (claim.priority > best_exact_priority) {
                best_exact_priority = claim.priority;
                best_exact_handler = claim.handler;
            } else if (claim.priority == best_exact_priority) {
                Logger::instance().warn("Conflicting stage claims for " +
                    game_id + "." + stage_name + " - same priority, keeping first");
            }
        }
    }

    // Exact-match wins at equal priority; higher priority always wins
    if (best_exact_handler && best_exact_priority >= best_wildcard_priority) {
        return best_exact_handler;
    }
    if (best_wildcard_handler && best_wildcard_priority > best_exact_priority) {
        return best_wildcard_handler;
    }
    return best_exact_handler ? best_exact_handler : best_wildcard_handler;
}

bool StageRegistry::has_claim(const std::string& game_id,
                               const std::string& stage_name) const {
    for (const auto& claim : claims_) {
        if (claim.stage_name == stage_name &&
            (claim.game_id == game_id || claim.game_id.empty())) {
            return true;
        }
    }
    return false;
}

void StageRegistry::clear() {
    claims_.clear();
}

}  // namespace engine
