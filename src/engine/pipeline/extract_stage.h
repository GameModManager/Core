#pragma once

#include <string>

#include "engine/pipeline/stage.h"

namespace engine {

class ExtractStage : public Stage {
public:
    bool execute(Mod& mod, PipelineContext& ctx) override;
    std::string name() const override { return "Extract"; }
    std::string description() const override {
        return "Unpacks the archive into the staging cache and reads metadata";
    }
    std::string condition() const override { return "Archive extracted to cache"; }
};

}  // namespace engine
