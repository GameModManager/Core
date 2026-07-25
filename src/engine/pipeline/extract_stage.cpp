#include "engine/pipeline/extract_stage.h"
#include "engine/pipeline/pipeline.h"

namespace engine {

bool ExtractStage::execute(Mod& mod, PipelineContext& ctx) {
    mod.state = ModState::Extracted;
    return true;
}

}  // namespace engine
