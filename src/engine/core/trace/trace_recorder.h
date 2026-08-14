#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine {

// Lifecycle status of a single pipeline stage.
enum class TraceStatus {
    Pending,        // declared, not yet started
    Running,        // executing right now
    Completed,      // finished successfully
    Failed,         // finished with an error
    Skipped,        // implemented but not reached (earlier stage failed)
    NotImplemented, // no implementation exists (core or plugin)
};

// One stage in a flow, as displayed by the pipeline window.
struct TraceStage {
    std::string name;
    std::string origin;      // "core" or the plugin id/path that provides it
    std::string description; // what this stage does (shown on the card)
    bool implemented = true;
    TraceStatus status = TraceStatus::Pending;
    std::string reason;      // exit condition, failure message, or skip reason
    int64_t duration_ms = 0;
    int64_t timestamp_ms = 0;  // steady_clock, ms
};

// Process-wide record of workflow pipelines (install / launch / sort).
// Qt-free and thread-safe: pipelines run on worker threads, the UI polls
// snapshots for display.  The UI is the only consumer; recording is cheap.
class TraceRecorder {
public:
    static TraceRecorder& instance();

    // Define the full ordered stage list for a flow, in order.
    // Replaces any previous declaration of the same flow id.
    void declare_flow(const std::string& flow_id,
                      const std::string& title,
                      std::vector<TraceStage> stages);

    // Start a new run of a declared flow: resets stage statuses, keeps the
    // declaration.  Creates a bare flow if never declared.
    void begin_flow(const std::string& flow_id);

    // Mark a stage as currently executing (creates it if not declared).
    void begin_stage(const std::string& flow_id, const std::string& stage_name);

    // Complete the stage that is currently executing in this flow.
    void end_stage(const std::string& flow_id, bool success,
                   const std::string& reason = {});

    // Finish a flow: remaining Pending stages become Skipped, the stage
    // currently running (if any) is finalized with success/failure.
    void end_flow(const std::string& flow_id, bool success,
                  const std::string& reason = {});

    // Snapshot of one flow for the UI. Thread-safe copy.
    struct FlowSnapshot {
        std::string flow_id;
        std::string title;
        bool running = false;
        bool started = false;  // begin_flow has been called at least once
        bool success = false;
        std::string current_stage;
        std::vector<TraceStage> stages;
    };
    [[nodiscard]] std::optional<FlowSnapshot> snapshot(const std::string& flow_id) const;

private:
    TraceRecorder() = default;

    struct FlowState {
        std::string flow_id;
        std::string title;
        bool running = false;
        bool started = false;
        bool success = false;
        std::string current_stage;
        std::vector<TraceStage> stages;
        std::unordered_map<std::string, size_t> stage_index;
    };

    static int64_t now_ms();
    static TraceStage* find_stage(FlowState& flow, const std::string& name);
    FlowState& flow_locked(const std::string& flow_id);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, FlowState> flows_;
};

}  // namespace engine
