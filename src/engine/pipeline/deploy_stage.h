#pragma once

#include <string>

#include "engine/pipeline/stage.h"

namespace engine {

class DeployStage : public Stage {
public:
    bool execute(Mod& mod, PipelineContext& ctx) override;
    std::string name() const override { return "Deploy"; }
    std::string description() const override {
        return "Links the mod's files into the game tree (or overlay staging)";
    }
    std::string condition() const override { return "Mod deployed into game tree"; }
};

}  // namespace engine
