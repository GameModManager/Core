// PipelineWorker download-pool regression test (P8.5 / T5).
//
// Pins the parallel-download contracts the fetch pool must hold:
//   1. Two simultaneous downloads run CONCURRENTLY (proven deterministically
//      via a 2-way barrier in the fake provider - both fetches are inside
//      fetch() at once, so max_active reaches exactly the pool size).
//   2. The pool is bounded (kMaxConcurrentDownloads): with two slots busy, a
//      third download QUEUES and only starts once a slot frees (FIFO).
//   3. Pausing an in-flight download aborts the transfer cooperatively, keeps
//      the partial file (the resume contract), and reports `paused` - never a
//      `download_complete`.
//   4. Pausing a QUEUED download (all slots busy, not yet started) drops it
//      from the queue, reports `paused`, and it never starts.
//   5. Beyond the pool cap, concurrency never exceeds kMaxConcurrentDownloads,
//      even when more downloads are queued than there are slots.
//   6. Per-source queueing (Settings -> Sources -> Nexus "Queue downloads"):
//      with nexus_queue_downloads ON, Nexus downloads run ONE at a time even
//      though a free slot exists, while a LoversLab download can still run
//      alongside; with it OFF, Nexus downloads parallelize again.
//
// Hermetic: QCoreApplication (no widgets), fake providers write local files,
// no network, throwaway temp dir.
#include "ui/pipeline_worker.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>

#include "engine/log/logger.h"
#include "engine/source/source_provider.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>
#include <catch2/catch_test_macros.hpp>

namespace {
void check(bool cond, const char* what) {
    INFO(what);
    REQUIRE(cond);
}
}

// A provider that "downloads" by writing local bytes with a polled-transfer
// loop, exactly like the real providers exercise the PipelineContext:
// should_abort is polled per chunk, and an abort sets download_paused and KEEPS
// the partial file. Optional 2-way barrier (set_barrier) holds every fetch
// until release_barrier() so the concurrency assertions are deterministic.
class FakeProvider : public engine::SourceProvider {
public:
    explicit FakeProvider(std::string source_type = "loverslab")
        : source_type_(std::move(source_type)) {}

    std::string source_type() const override { return source_type_; }

    bool fetch(const engine::Mod&, engine::PipelineContext& ctx,
               const std::filesystem::path& dest_path) override {
        started_.fetch_add(1);
        {
            std::lock_guard<std::mutex> lk(entry_mu_);
            entered_.fetch_add(1);
            entry_cv_.notify_all();
        }

        if (barrier_.load()) {
            std::unique_lock<std::mutex> lk(barrier_mu_);
            if (!barrier_cv_.wait_for(lk, std::chrono::seconds(5),
                                      [this] { return barrier_go_.load(); })) {
                // 5s with no release = the test is wedged (or the pool failed
                // to start the other transfer). Fail loudly, don't hang.
                return false;
            }
        }

        const int active_now = active_.fetch_add(1) + 1;
        {
            std::lock_guard<std::mutex> lk(max_mu_);
            if (active_now > max_active_)
                max_active_ = active_now;
        }

        // Slow polled transfer: ~100 chunks x 5ms = ~500ms, long enough for
        // the pause and queue windows to be observed. Abort keeps the partial
        // file. Flush every chunk so the partial file's on-disk size grows
        // incrementally (the test observes bytes-before-pause).
        std::ofstream out(dest_path, std::ios::binary | std::ios::trunc);
        for (int i = 0; i < 100; ++i) {
            if (ctx.should_abort && ctx.should_abort()) {
                ctx.download_paused = true;
                out.close();
                active_.fetch_sub(1);
                return false;
            }
            out.write("payload!", 8);
            out.flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        out.close();
        active_.fetch_sub(1);
        completed_.fetch_add(1);
        return true;
    }

    // Re-arm the barrier. When on, every fetch blocks until release_barrier();
    // when off, fetches never block. entered_ resets so wait_entered() counts
    // only the current test's downloads.
    void set_barrier(bool on) {
        std::lock_guard<std::mutex> lk(barrier_mu_);
        barrier_.store(on);
        barrier_go_.store(!on);
        entered_.store(0);
    }
    void release_barrier() {
        std::lock_guard<std::mutex> lk(barrier_mu_);
        barrier_go_.store(true);
        barrier_cv_.notify_all();
    }
    // Block the calling (test) thread until `n` fetches have entered fetch()
    // since the last set_barrier().
    bool wait_entered(int n, int timeout_ms) {
        std::unique_lock<std::mutex> lk(entry_mu_);
        return entry_cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                                  [this, n] { return entered_.load() >= n; });
    }
    int started() const { return started_.load(); }
    int completed() const { return completed_.load(); }
    int entered() const { return entered_.load(); }
    int max_active() const {
        std::lock_guard<std::mutex> lk(max_mu_);
        return max_active_;
    }

private:
    std::atomic<int> started_{0};
    std::atomic<int> completed_{0};
    std::atomic<int> active_{0};
    std::atomic<int> entered_{0};
    mutable std::mutex max_mu_;
    int max_active_ = 0;
    std::mutex entry_mu_;
    std::condition_variable entry_cv_;
    std::atomic<bool> barrier_{false};
    std::atomic<bool> barrier_go_{true};
    std::mutex barrier_mu_;
    std::condition_variable barrier_cv_;
    std::string source_type_;
};

// Pump the main-thread event loop until `cond` is true (queued signal delivery
// to the main thread only happens through processEvents) or the timeout hits.
static bool wait_until(const std::function<bool()>& cond, int timeout_ms) {
    QElapsedTimer timer;
    timer.start();
    while (!cond()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        if (timer.elapsed() > timeout_ms)
            return false;
        QThread::msleep(2);
    }
    return true;
}

TEST_CASE("pipeline worker", "[ui]") {
    int test_argc = 1;
    char test_argv0[] = "test";
    char* test_argv[] = {test_argv0, nullptr};
    QCoreApplication app(test_argc, test_argv);
    (void)app;
    engine::Logger::instance().enable_console();

    const std::filesystem::path base =
        std::filesystem::current_path() /
        ("gmm_test_pipeline_worker_" + std::to_string(getpid()));
    const std::filesystem::path mods = base / "mods";
    const std::filesystem::path meta = base / "meta";
    const std::filesystem::path downloads = base / "downloads";
    std::error_code ec;
    std::filesystem::create_directories(mods, ec);

    auto provider = std::make_unique<FakeProvider>("loverslab");
    FakeProvider* fake = provider.get();
    engine::SourceRegistry::instance().register_provider(std::move(provider));
    // Second provider for the "nexus" source type so per-source queueing is
    // exercised through the real FetchStage routing (download_mod sets
    // download_source_type == "nexus"). Its barrier/counters are independent.
    auto nexus_provider = std::make_unique<FakeProvider>("nexus");
    FakeProvider* nfake = nexus_provider.get();
    engine::SourceRegistry::instance().register_provider(std::move(nexus_provider));

    // Result collectors. Context = the app object so auto connections queue
    // signal delivery onto the main thread (no cross-thread vector writes).
    std::vector<std::pair<std::string, bool>> completions;
    std::vector<std::string> paused_ids;
    ui::PipelineWorker worker;
    QObject::connect(&worker, &ui::PipelineWorker::download_complete,
                     &app, [&](const std::string& id, bool ok,
                               const std::string&, const std::string&) {
        completions.emplace_back(id, ok);
    });
    QObject::connect(&worker, &ui::PipelineWorker::paused,
                     &app, [&](const std::string& id) {
        paused_ids.push_back(id);
    });

    const auto start = [&](const std::string& id) {
        worker.download_mod_url(id, "http://fake.example/" + id, "fake-game",
                                mods.string(), meta.string());
    };
    // A Nexus download via download_mod (real NxmLink path -> source_type
    // "nexus"). Distinct mod_ids keep the archive filenames unique.
    const auto start_nexus = [&](const std::string& id, int64_t mod_id) {
        engine::NxmLink link;
        link.nexus_domain = "skyrimspecialedition";
        link.mod_id = mod_id;
        link.file_id = 1;
        worker.download_mod(id, link, "skyrim", mods.string(), meta.string());
    };
    const auto has_completion = [&](const std::string& id) {
        for (const auto& [cid, ok] : completions)
            if (cid == id) return true;
        return false;
    };
    const auto saw_pause = [&](const std::string& id) {
        for (const auto& pid : paused_ids)
            if (pid == id) return true;
        return false;
    };

    // --- 1) Two downloads run concurrently (deterministic barrier overlap).
    fake->set_barrier(true);
    start("a");
    start("b");
    check(fake->wait_entered(2, 5000),
          "both downloads enter fetch() (two slots start immediately)");
    check(fake->started() == 2, "exactly the two dispatched downloads started");
    fake->release_barrier();
    check(wait_until([&] { return fake->completed() == 2; }, 5000),
          "both concurrent downloads complete");
    check(fake->max_active() == ui::PipelineWorker::kMaxConcurrentDownloads,
          "the two transfers were in flight at the same time (parallel pool)");
    check(fake->started() == 2 && fake->completed() == 2,
          "no extra fetches after the two completed");
    check(std::filesystem::exists(downloads / "a.zip") &&
              std::filesystem::exists(downloads / "b.zip"),
          "both archives landed in the instance downloads dir");
    check(wait_until([&] { return has_completion("a") && has_completion("b"); }, 5000),
          "download_complete emitted for both");

    // --- 2) Bounded pool + FIFO queue: a third download waits for a slot.
    fake->set_barrier(true);
    start("q1");
    start("q2");
    check(fake->wait_entered(2, 5000),
          "queue test: first two downloads occupy both slots");
    start("q3");
    // Give a (buggy) pool a long window to wrongly start q3; it must not.
    QThread::msleep(150);
    check(fake->started() == 4, "third download stays QUEUED while slots are busy");
    fake->release_barrier();
    check(wait_until([&] { return fake->completed() == 5; }, 5000),
          "queued download starts and completes after a slot frees");
    check(wait_until([&] { return has_completion("q3"); }, 5000),
          "download_complete emitted for the queued download");
    check(fake->max_active() == ui::PipelineWorker::kMaxConcurrentDownloads,
          "concurrency never exceeded the pool cap while queueing");

    // --- 3) Pause an in-flight download: cooperative abort, partial kept.
    fake->set_barrier(false);
    const std::filesystem::path partial = downloads / "p.zip";
    start("p");
    check(fake->wait_entered(1, 5000), "paused download started");
    check(wait_until([&] {
              return std::filesystem::exists(partial) &&
                     std::filesystem::file_size(partial) > 0;
          }, 5000),
          "partial bytes are flowing before the pause");
    worker.pause_download("p");
    check(wait_until([&] { return saw_pause("p"); }, 5000),
          "pausing an in-flight download emits paused (not download_complete)");
    check(!has_completion("p"), "paused download never reports complete");
    check(std::filesystem::exists(partial) &&
              std::filesystem::file_size(partial) > 0,
          "partial file is kept on disk for a later resume (Range)");

    // --- 4) Pause a QUEUED download (all slots busy, never started).
    fake->set_barrier(true);
    start("r1");
    start("r2");
    check(fake->wait_entered(2, 5000),
          "queue-pause test: both slots occupied");
    start("r3");
    QThread::msleep(150);
    check(fake->started() == 8, "queued download not started before pause");
    worker.pause_download("r3");
    check(wait_until([&] { return saw_pause("r3"); }, 5000),
          "pausing a queued download emits paused");
    fake->release_barrier();
    check(wait_until([&] { return fake->completed() == 7; }, 5000),
          "the two running downloads complete");
    check(wait_until([&] { return !has_completion("r3"); }, 300),
          "the paused-queued download never ran nor completed");
    check(fake->started() == 8 && fake->completed() == 7,
          "paused-queued download stayed dropped (not resumed by the freed slot)");

    // --- 5) Nexus queueing ON: two Nexus downloads run ONE at a time. ---
    worker.set_nexus_queue_downloads(true);
    nfake->set_barrier(true);
    start_nexus("n1", 1001);
    start_nexus("n2", 1002);
    // Poll with event pumping: this harness runs PipelineWorker on the main
    // thread, so the queued slot-freeing that drains pending_ is delivered via
    // processEvents. (The real app runs the worker on its own thread, where
    // dispatch and slot-freeing share one event loop.)
    check(wait_until([&] { return nfake->entered() >= 1; }, 5000),
          "queueing: first nexus download starts");
    QThread::msleep(150);
    check(nfake->started() == 1,
          "queueing: second nexus download stays QUEUED (1-at-a-time) even with a free slot");
    nfake->release_barrier();
    check(wait_until([&] { return nfake->completed() == 1; }, 5000),
          "queueing: first nexus download completes");
    check(wait_until([&] { return nfake->started() == 2; }, 5000),
          "queueing: second nexus download starts only after the first finished");
    nfake->release_barrier();
    check(wait_until([&] { return nfake->completed() == 2; }, 5000),
          "queueing: both nexus downloads complete");
    check(nfake->max_active() == 1,
          "queueing: nexus transfers never overlapped");
    check(wait_until([&] { return has_completion("n1") && has_completion("n2"); }, 5000),
          "queueing: download_complete emitted for both nexus downloads");
    check(std::filesystem::exists(downloads / "1001-1.zip") &&
              std::filesystem::exists(downloads / "1002-1.zip"),
          "queueing: both nexus archives landed in the downloads dir");

    // --- 6) Cross-source: nexus queueing never blocks other sources. ---
    worker.set_nexus_queue_downloads(true);
    nfake->set_barrier(true);
    fake->set_barrier(true);
    start_nexus("n3", 1003);
    start("l1");
    check(wait_until([&] { return nfake->entered() >= 1; }, 5000),
          "cross-source: nexus download is running");
    check(wait_until([&] { return fake->entered() >= 1; }, 5000),
          "cross-source: a LoversLab download starts WHILE the nexus one is active");
    fake->release_barrier();
    nfake->release_barrier();
    check(wait_until([&] { return has_completion("l1") && has_completion("n3"); }, 5000),
          "cross-source: both downloads complete");
    check(nfake->max_active() == 1,
          "cross-source: the nexus side still never overlapped");

    // --- 7) Queueing OFF: Nexus downloads run concurrently again. ---
    worker.set_nexus_queue_downloads(false);
    nfake->set_barrier(true);
    start_nexus("n4", 1004);
    start_nexus("n5", 1005);
    check(wait_until([&] { return nfake->entered() >= 2; }, 5000),
          "queueing off: both nexus downloads enter fetch() at once");
    nfake->release_barrier();
    check(wait_until([&] { return nfake->completed() == 5; }, 5000),
          "queueing off: both nexus downloads complete");
    check(nfake->max_active() == 2,
          "queueing off: nexus transfers ran in parallel again");
    check(wait_until([&] { return has_completion("n4") && has_completion("n5"); }, 5000),
          "queueing off: download_complete emitted for both");

    std::filesystem::remove_all(base, ec);
}
