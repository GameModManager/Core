#include "engine/pipeline/install_stage.h"
#include "engine/pipeline/pipeline.h"

namespace engine {

bool InstallStage::execute(Mod& mod, PipelineContext& ctx) {
    mod.state = ModState::Installed;
    return true;
}

}  // namespace engine
