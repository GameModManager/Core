#include "engine/registry/stage_registry.h"
#include "engine/log/logger.h"

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
    StageFn best_handler;
    int best_priority = INT32_MIN;

    for (const auto& claim : claims_) {
        if (claim.game_id == game_id && claim.stage_name == stage_name) {
            if (claim.priority > best_priority) {
                best_priority = claim.priority;
                best_handler = claim.handler;
            } else if (claim.priority == best_priority) {
                // Log conflict
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
        if (claim.game_id == game_id && claim.stage_name == stage_name) {
            return true;
        }
    }
    return false;
}

void StageRegistry::clear() {
    claims_.clear();
}

}  // namespace engine
