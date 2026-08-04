#include "engine/pipeline/pipeline.h"

#include "engine/trace/trace_recorder.h"

namespace engine {

void Pipeline::set_context(PipelineContext ctx) {
    ctx_ = std::move(ctx);
}

void Pipeline::add_stage(std::unique_ptr<Stage> stage) {
    stages_.push_back(std::move(stage));
}

PipelineResult Pipeline::run(Mod& mod) {
    auto& trace = TraceRecorder::instance();
    trace.begin_flow(flow_id_);

    for (auto& stage : stages_) {
        auto name = stage->name();
        if (name.empty()) name = "?";
        trace.begin_stage(flow_id_, name);

        bool ok = stage->execute(mod, ctx_);
        if (!ok) {
            if (ctx_.canceled) {
                trace.end_stage(flow_id_, false, "Canceled by user");
                trace.end_flow(flow_id_, false, "Pipeline canceled at " + name);
            } else {
                trace.end_stage(flow_id_, false, "Stage failed");
                trace.end_flow(flow_id_, false, "Pipeline stopped at " + name);
            }
            // Best-effort cleanup of the extract staging dir so a canceled or
            // failed install never leaves <archive>_tmp behind.
            std::error_code ec;
            for (const auto& f : mod.files) {
                auto p = std::filesystem::path(f.relative_path);
                if (std::filesystem::is_directory(p, ec))
                    std::filesystem::remove_all(p, ec);
            }
            return ctx_.canceled ? PipelineResult::Canceled : PipelineResult::Failed;
        }
        trace.end_stage(flow_id_, true, stage->condition());
    }

    trace.end_flow(flow_id_, true);
    return PipelineResult::Success;
}

}  // namespace engine
