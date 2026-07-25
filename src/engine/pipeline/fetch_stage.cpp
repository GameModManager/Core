#include "engine/pipeline/fetch_stage.h"
#include "engine/pipeline/pipeline.h"

namespace engine {

bool FetchStage::execute(Mod& mod, PipelineContext& ctx) {
    mod.state = ModState::Downloaded;
    return true;
}

}  // namespace engine
