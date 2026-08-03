#pragma once

#include <string>

#include "engine/pipeline/stage.h"

namespace engine {

// Detects FOMOD installer archives (a fomod/ directory containing
// ModuleConfig.xml) and drives the full FOMOD install flow: parse the
// ModuleConfig + info.xml, build the FomodViewModel, ask the user (or restore
// previously persisted choices headlessly), apply the chosen options to the
// extracted staging dir, and pass the choices JSON on to InstallStage for
// meta.ini persistence.
class FomodStage : public Stage {
public:
    bool execute(Mod& mod, PipelineContext& ctx) override;
    std::string name() const override { return "Fomod"; }
    std::string description() const override {
        return "Detects FOMOD installer archives (fomod/ModuleConfig.xml) and "
               "installs them via the FOMOD installer wizard";
    }
    std::string condition() const override { return "Not a FOMOD archive"; }
};

}  // namespace engine
