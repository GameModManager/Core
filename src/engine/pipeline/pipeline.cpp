#include "engine/pipeline/pipeline.h"

#include "engine/trace/trace_recorder.h"

namespace engine {

void Pipeline::set_context(PipelineContext ctx) {
    ctx_ = std::move(ctx);
}

void Pipeline::add_stage(std::unique_ptr<Stage> stage) {
    stages_.push_back(std::move(stage));
}

bool Pipeline::run(Mod& mod) {
    auto& trace = TraceRecorder::instance();
    trace.begin_flow(flow_id_);

    for (auto& stage : stages_) {
        auto name = stage->name();
        if (name.empty()) name = "?";
        trace.begin_stage(flow_id_, name);

        bool ok = stage->execute(mod, ctx_);
        if (!ok) {
            trace.end_stage(flow_id_, false, "Stage failed");
            trace.end_flow(flow_id_, false, "Pipeline stopped at " + name);
            return false;
        }
        trace.end_stage(flow_id_, true, stage->condition());
    }

    trace.end_flow(flow_id_, true);
    return true;
}

}  // namespace engine
