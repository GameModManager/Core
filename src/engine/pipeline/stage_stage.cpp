#include "engine/pipeline/stage_stage.h"
#include "engine/pipeline/pipeline.h"

namespace engine {

bool StageStage::execute(Mod& mod, PipelineContext& ctx) {
    mod.state = ModState::Staged;
    return true;
}

}  // namespace engine
