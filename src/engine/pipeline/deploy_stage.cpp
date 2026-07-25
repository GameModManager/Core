#include "engine/pipeline/deploy_stage.h"
#include "engine/pipeline/pipeline.h"

namespace engine {

bool DeployStage::execute(Mod& mod, PipelineContext& ctx) {
    mod.state = ModState::Deployed;
    return true;
}

}  // namespace engine
