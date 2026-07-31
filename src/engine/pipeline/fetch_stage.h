#pragma once

#include <string>

#include "engine/pipeline/stage.h"

namespace engine {

class FetchStage : public Stage {
public:
    bool execute(Mod& mod, PipelineContext& ctx) override;
    std::string name() const override { return "Fetch"; }
    std::string description() const override {
        return "Downloads the mod archive into the instance download cache";
    }
    std::string condition() const override { return "Archive downloaded"; }
};

}  // namespace engine
