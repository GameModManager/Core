#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine {

struct Mod;
struct PipelineContext;

// Stage execution function
using StageFn = std::function<bool(Mod&, PipelineContext&)>;

struct StageClaim {
    std::string game_id;
    std::string stage_name;
    StageFn handler;
    int priority = 0;
    std::string plugin_id;  // for logging which plugin claimed it
};

class StageRegistry {
public:
    void register_claim(const std::string& game_id,
                        const std::string& stage_name,
                        StageFn handler,
                        int priority = 0,
                        const std::string& plugin_id = "");

    // Returns the handler for a (game_id, stage_name) pair, or nullptr if none claimed
    [[nodiscard]] StageFn get_handler(const std::string& game_id,
                                      const std::string& stage_name) const;

    // Check if any claim exists for a given stage
    [[nodiscard]] bool has_claim(const std::string& game_id,
                                 const std::string& stage_name) const;

    // Get all claims for a game (for logging/debugging)
    [[nodiscard]] const std::vector<StageClaim>& claims() const { return claims_; }

    void clear();

private:
    std::vector<StageClaim> claims_;
};

}  // namespace engine
