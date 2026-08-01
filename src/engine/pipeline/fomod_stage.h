#pragma once

#include <string>

#include "engine/pipeline/stage.h"

namespace engine {

class FomodStage : public Stage {
public:
    bool execute(Mod& mod, PipelineContext& ctx) override;
    std::string name() const override { return "Fomod"; }
    std::string description() const override {
        return "Detects FOMOD installer archives (fomod/ModuleConfig.xml); "
               "FOMOD installers are not implemented yet";
    }
    std::string condition() const override { return "Not a FOMOD archive"; }
};

}  // namespace engine
