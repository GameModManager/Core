#include "engine/core/trace/trace_recorder.h"

#include <chrono>

namespace engine {

TraceRecorder& TraceRecorder::instance() {
    static TraceRecorder recorder;
    return recorder;
}

int64_t TraceRecorder::now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

TraceRecorder::FlowState& TraceRecorder::flow_locked(const std::string& flow_id) {
    auto it = flows_.find(flow_id);
    if (it != flows_.end()) return it->second;

    FlowState state;
    state.flow_id = flow_id;
    state.title = flow_id;
    auto [iter, _] = flows_.emplace(flow_id, std::move(state));
    return iter->second;
}

TraceStage* TraceRecorder::find_stage(FlowState& flow, const std::string& name) {
    auto it = flow.stage_index.find(name);
    if (it == flow.stage_index.end() || it->second >= flow.stages.size())
        return nullptr;
    return &flow.stages[it->second];
}

void TraceRecorder::declare_flow(const std::string& flow_id,
                                 const std::string& title,
                                 std::vector<TraceStage> stages) {
    std::lock_guard lock(mutex_);
    auto& flow = flow_locked(flow_id);
    flow.title = title.empty() ? flow_id : title;
    flow.stages = std::move(stages);
    flow.stage_index.clear();
    for (size_t i = 0; i < flow.stages.size(); ++i) {
        flow.stage_index[flow.stages[i].name] = i;
        // Normalize status from the declared `implemented` flag so a fresh
        // declaration renders correctly (greyed out) before any run.
        flow.stages[i].status = flow.stages[i].implemented
                                    ? TraceStatus::Pending
                                    : TraceStatus::NotImplemented;
    }
    flow.running = false;
    flow.success = false;
    flow.current_stage.clear();
    flow.started = false;
}

void TraceRecorder::begin_flow(const std::string& flow_id) {
    std::lock_guard lock(mutex_);
    auto& flow = flow_locked(flow_id);
    flow.running = true;
    flow.started = true;
    flow.success = false;
    flow.current_stage.clear();
    auto now = now_ms();
    for (auto& stage : flow.stages) {
        stage.status = stage.implemented ? TraceStatus::Pending
                                         : TraceStatus::NotImplemented;
        stage.reason.clear();
        stage.duration_ms = 0;
        stage.timestamp_ms = now;
    }
}

void TraceRecorder::begin_stage(const std::string& flow_id,
                                const std::string& stage_name) {
    std::lock_guard lock(mutex_);
    auto& flow = flow_locked(flow_id);
    auto* stage = find_stage(flow, stage_name);
    if (!stage) {
        // Undeclared stage - append dynamically (launch/sort flows may vary).
        TraceStage s;
        s.name = stage_name;
        s.origin = "core";
        s.status = TraceStatus::Running;
        s.timestamp_ms = now_ms();
        flow.stage_index[stage_name] = flow.stages.size();
        flow.stages.push_back(std::move(s));
        stage = &flow.stages.back();
    }
    flow.current_stage = stage_name;
    stage->status = TraceStatus::Running;
    stage->timestamp_ms = now_ms();
    stage->duration_ms = 0;
}

void TraceRecorder::end_stage(const std::string& flow_id, bool success,
                              const std::string& reason) {
    std::lock_guard lock(mutex_);
    auto& flow = flow_locked(flow_id);
    if (flow.current_stage.empty()) return;
    auto* stage = find_stage(flow, flow.current_stage);
    if (!stage) return;
    stage->status = success ? TraceStatus::Completed : TraceStatus::Failed;
    stage->duration_ms = now_ms() - stage->timestamp_ms;
    stage->reason = reason;
    flow.current_stage.clear();
}

void TraceRecorder::end_flow(const std::string& flow_id, bool success,
                             const std::string& reason) {
    std::lock_guard lock(mutex_);
    auto& flow = flow_locked(flow_id);
    flow.running = false;
    flow.success = success;
    flow.current_stage.clear();
    auto now = now_ms();
    for (auto& stage : flow.stages) {
        if (stage.status == TraceStatus::Running) {
            stage.status = success ? TraceStatus::Completed : TraceStatus::Failed;
            stage.duration_ms = now - stage.timestamp_ms;
            if (!reason.empty()) stage.reason = reason;
        } else if (stage.status == TraceStatus::Pending) {
            stage.status = TraceStatus::Skipped;
            stage.reason = "Not reached";
        }
    }
}

std::optional<TraceRecorder::FlowSnapshot> TraceRecorder::snapshot(
    const std::string& flow_id) const {
    std::lock_guard lock(mutex_);
    auto it = flows_.find(flow_id);
    if (it == flows_.end()) return std::nullopt;
    const auto& flow = it->second;

    FlowSnapshot snap;
    snap.flow_id = flow.flow_id;
    snap.title = flow.title;
    snap.running = flow.running;
    snap.started = flow.started;
    snap.success = flow.success;
    snap.current_stage = flow.current_stage;
    snap.stages = flow.stages;
    return snap;
}

}  // namespace engine
