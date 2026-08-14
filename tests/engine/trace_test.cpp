#include "engine/core/trace/trace_recorder.h"

#include <cstdio>
#include <string>
#include <thread>
#include <catch2/catch_test_macros.hpp>

using namespace engine;

namespace {
void require(bool cond, const char* msg) {
    INFO(msg);
    REQUIRE(cond);
}
}

TEST_CASE("trace", "[engine]") {
    auto& trace = TraceRecorder::instance();

    // --- declare_flow + snapshot basics ---
    trace.declare_flow("install", "Mod install pipeline", {
        {"Fetch", "core", "", true},
        {"Extract", "core", "", true},
        {"Install", "core", "", true},
        {"Stage", "plugin.so", "", true},
        {"Resolve", "core", "", false},  // not implemented
    });
    auto snap = trace.snapshot("install");
    require(snap.has_value(), "snapshot exists after declare_flow");
    require(snap->stages.size() == 5, "all declared stages present");
    require(snap->stages[0].name == "Fetch", "stage order preserved");
    require(snap->stages[4].status == TraceStatus::NotImplemented,
           "unimplemented stage starts as NotImplemented");
    require(!snap->running, "flow not running before begin_flow");
    require(!snap->started, "flow not started before begin_flow");

    // --- begin_flow resets statuses, keeps declaration ---
    trace.begin_stage("install", "Fetch");          // dirty the state
    trace.end_stage("install", true);
    trace.begin_flow("install");
    snap = trace.snapshot("install");
    require(snap->running, "flow running after begin_flow");
    require(snap->started, "flow marked started after begin_flow");
    require(snap->stages[0].status == TraceStatus::Pending, "statuses reset on begin_flow");
    require(snap->stages[4].status == TraceStatus::NotImplemented,
           "implemented flag survives begin_flow");

    // --- stage lifecycle ---
    trace.begin_stage("install", "Fetch");
    snap = trace.snapshot("install");
    require(snap->current_stage == "Fetch", "current_stage set");
    require(snap->stages[0].status == TraceStatus::Running, "stage marked Running");
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    trace.end_stage("install", true, "Archive downloaded");
    snap = trace.snapshot("install");
    require(snap->stages[0].status == TraceStatus::Completed, "successful stage Completed");
    require(snap->stages[0].reason == "Archive downloaded", "exit condition recorded");
    require(snap->stages[0].duration_ms >= 0, "duration recorded");

    // --- failure + end_flow marks remaining stages skipped ---
    trace.begin_stage("install", "Extract");
    trace.end_stage("install", false, "Corrupt archive");
    trace.end_flow("install", false, "Pipeline stopped at Extract");
    snap = trace.snapshot("install");
    require(!snap->running, "flow ends running on end_flow");
    require(!snap->success, "flow success reflects failure");
    require(snap->stages[1].status == TraceStatus::Failed, "failed stage Failed");
    require(snap->stages[2].status == TraceStatus::Skipped, "next stage Skipped");
    require(snap->stages[3].status == TraceStatus::Skipped, "later stage Skipped");
    require(snap->stages[4].status == TraceStatus::NotImplemented,
           "unimplemented stage never Skipped");

    // --- end_flow finalizes a still-running stage ---
    trace.begin_flow("install");
    trace.begin_stage("install", "Fetch");
    trace.end_flow("install", false, "Canceled mid-stage");
    snap = trace.snapshot("install");
    require(snap->stages[0].status == TraceStatus::Failed,
           "running stage finalized as Failed by end_flow");
    require(snap->stages[1].status == TraceStatus::Skipped, "remaining stages Skipped");

    // --- unknown stage appended dynamically ---
    trace.begin_flow("install");
    trace.begin_stage("install", "PhantomStage");
    snap = trace.snapshot("install");
    require(snap->stages.size() == 6, "undeclared stage appended");
    require(snap->stages[5].name == "PhantomStage", "appended stage named correctly");
    trace.end_stage("install", true);

    // --- undeclared flow created bare ---
    trace.begin_flow("never_declared");
    trace.end_flow("never_declared", true);
    snap = trace.snapshot("never_declared");
    require(snap.has_value(), "undeclared flow snapshot exists");
    require(snap->success, "bare flow ends successful");

    // --- flows are independent ---
    trace.begin_flow("sort");
    trace.begin_stage("sort", "Gather mod info");
    trace.end_stage("sort", true);
    trace.end_flow("sort", true);
    snap = trace.snapshot("install");
    require(snap->stages[0].status == TraceStatus::Pending,
           "install flow untouched by sort flow");

    // --- declared-but-never-run flow reports not started ---
    trace.declare_flow("launch", "Game launch", {
        {"Sync disk order", "core"},
        {"Prepare launch environment", "core"},
        {"Launch executable", "core"},
        {"Monitor process", "core"},
    });
    snap = trace.snapshot("launch");
    require(snap.has_value(), "eagerly declared flow has a snapshot");
    require(snap->stages.size() == 4, "declared flow has its full stage list");
    require(!snap->started, "declared-but-never-run flow not started");
    require(!snap->running, "declared-but-never-run flow not running");
    require(snap->stages[0].status == TraceStatus::Pending, "declared stages Pending");
    trace.begin_flow("launch");
    snap = trace.snapshot("launch");
    require(snap->started, "begin_flow marks flow started");

    // --- thread-safety smoke: concurrent stages on different flows ---
    std::thread t1([&] {
        for (int i = 0; i < 50; ++i) {
            trace.begin_stage("sort", "Tick");
            trace.end_stage("sort", true);
        }
    });
    std::thread t2([&] {
        for (int i = 0; i < 50; ++i) {
            trace.begin_stage("install", "Tick");
            trace.end_stage("install", true);
        }
    });
    t1.join();
    t2.join();
}
