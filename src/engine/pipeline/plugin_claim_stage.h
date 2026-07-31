#pragma once

#include <string>

#include "engine/pipeline/stage.h"
#include "engine/registry/stage_registry.h"

namespace engine {

// Wraps a plugin-registered stage handler as a Stage so plugin claims
// execute inside the same Pipeline machinery as core stages.
class PluginClaimStage : public Stage {
public:
    PluginClaimStage(std::string stage_name, std::string origin, StageFn handler);

    bool execute(Mod& mod, PipelineContext& ctx) override;
    std::string name() const override;
    std::string description() const override;
    std::string condition() const override;

private:
    std::string stage_name_;
    std::string origin_;
    StageFn handler_;
};

}  // namespace engine
