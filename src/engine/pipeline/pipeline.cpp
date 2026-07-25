#include "engine/pipeline/pipeline.h"

namespace engine {

void Pipeline::set_context(PipelineContext ctx) {
    ctx_ = std::move(ctx);
}

void Pipeline::add_stage(std::unique_ptr<Stage> stage) {
    stages_.push_back(std::move(stage));
}

bool Pipeline::run(Mod& mod) {
    for (auto& stage : stages_) {
        if (!stage->execute(mod, ctx_)) {
            return false;
        }
    }
    return true;
}

}  // namespace engine
