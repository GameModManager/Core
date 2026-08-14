#pragma once

#include "engine/mod/model/mod.h"
#include "engine/pipeline/pipeline.h"
#include "engine/source/nxm/nxm_router.h"

#include <QObject>
#include <QThread>
#include <atomic>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace engine {
class Pipeline;
}

namespace ui {

// One concurrent download slot. Owns its own fetch-only pipeline AND context -
// a PipelineContext is per-run mutable state (should_abort, resume offset,
// progress callbacks, paused flag), so two in-flight transfers must never share
// one. The actual transfer runs on this runner's own QThread; the worker thread
// only dispatches into the pool and relays signals, so it never blocks on a
// fetch. Pause/resume contract is identical to PipelineWorker::download_mod.
class FetchRunner : public QObject {
    Q_OBJECT
public:
    explicit FetchRunner(QObject* parent = nullptr);
    ~FetchRunner();

    // Cooperative pause flag, polled by the transfer callback. Written from the
    // worker thread (dispatch/pause), read from this runner's thread.
    std::atomic_bool& cancel_flag() { return cancel_flag_; }
    // True while this slot has a download assigned. Read/written only on the
    // worker thread (dispatch selects free slots, fetch_finished frees them).
    std::atomic_bool& busy() { return busy_; }

    // quit() the slot thread and wait (bounded) for the in-flight transfer to
    // abort. Called from the worker's destructor / stop path.
    void stop();

public slots:
    // Run one fetch (download only): produces the archive in the instance
    // downloads dir, then emits download_complete / paused. Snapshot-by-value
    // input (THREADING §3.5); runs on this runner's own thread.
    void run(const std::string& id, engine::Mod mod,
             const std::string& mods_dir, const std::string& meta_dir);

signals:
    void download_progress(const std::string& mod_id, int64_t bytes_downloaded,
                           int64_t bytes_total, double speed_bytes_per_sec);
    void download_meta(const std::string& mod_id,
                       const std::string& archive_name,
                       const std::string& display_name);
    void download_complete(const std::string& mod_id, bool success,
                           const std::string& archive_path,
                           const std::string& name);
    void paused(const std::string& mod_id);
    // Emitted on the runner thread when run() returns (for any terminal
    // outcome). The worker listens for it to free the slot and dequeue the next
    // queued download.
    void fetch_finished(const std::string& id);

private:
    std::unique_ptr<engine::Pipeline> fetch_pipeline_;
    QThread* thread_ = nullptr;
    std::atomic_bool cancel_flag_{false};
    std::atomic_bool busy_{false};
};

class PipelineWorker : public QObject {
    Q_OBJECT
public:
    // Max concurrent downloads (MO2's default pool size). Downloads are
    // decoupled from installs and run in the background, so a pool of this
    // many simultaneous transfers is the throughput ceiling on multi-file
    // installs; the queue beyond it waits for a free slot.
    static constexpr int kMaxConcurrentDownloads = 2;

    explicit PipelineWorker(QObject* parent = nullptr);
    ~PipelineWorker();

    void set_pipeline(std::unique_ptr<engine::Pipeline> pipeline);
    void set_context(engine::PipelineContext ctx);

    // When on, Nexus downloads run one-at-a-time (free Regular/Supporter
    // accounts are throttled to ~1.5MB/s, so parallel transfers don't help;
    // Premium lifts the cap). Other sources keep the pool's parallel slots, so
    // a LoversLab download can still run alongside a single Nexus one. Written
    // from the main thread (startup + after the settings dialog closes), read
    // from the worker thread - hence the atomic.
    void set_nexus_queue_downloads(bool on) { nexus_queue_downloads_.store(on); }
    bool nexus_queue_downloads() const { return nexus_queue_downloads_.load(); }

public slots:
    // Install an already-downloaded archive (downloads are decoupled from
    // installs). source_type/source_id/file_id preserve the origin metadata
    // for meta.ini (e.g. "nexus" + parent mod id + file id); page_url is the
    // source page URL (LoversLab) that lands in the mod's per-source section.
    // name is the display name from the Downloads tab (e.g. "SkyUI") and
    // becomes the mod folder name; empty falls back to the download id.
    void install_mod(const std::string& id, const std::string& zip_path,
                     const std::string& source_type = {},
                     const std::string& source_id = {}, int file_id = 0,
                     const std::string& name = {},
                     const std::string& page_url = {});

    // Download only (fetch stage): produces the archive in the instance
    // downloads dir, then emits download_complete. Resume-safe - a partial
    // file from a paused download is continued via HTTP Range. Concurrent
    // downloads run on the fetch pool (kMaxConcurrentDownloads transfers in
    // parallel); excess downloads queue until a slot frees.
    void download_mod(const std::string& id, const engine::NxmLink& link,
                      const std::string& game_id,
                      const std::string& mods_dir,
                      const std::string& meta_dir);

    // Download only, by pre-assembled URL (LoversLab and other no-API sites).
    // The provider fetches mod.download_url with the configured session
    // cookie. Same pause/progress/resume contract as download_mod.
    void download_mod_url(const std::string& id, const std::string& url,
                          const std::string& game_id,
                          const std::string& mods_dir,
                          const std::string& meta_dir);

    // Request a pause of an in-flight download (cooperative: the transfer
    // callback polls the flag and aborts, keeping the partial file). Also
    // handles a download that is still queued behind a busy pool - it is
    // dropped from the queue and reported paused (nothing was fetched).
    void pause_download(const std::string& id);

signals:
    void progress(const std::string& mod_id, int stage_index, const std::string& stage_name);
    void download_progress(const std::string& mod_id, int64_t bytes_downloaded, int64_t bytes_total, double speed_bytes_per_sec);
    // Per-stage install progress (extract/copy). percent is 0-100, or -1 for
    // a stage that cannot estimate progress (indeterminate bar); status is a
    // short human line ("Extracting SkyUI.zip…", "Installing to SkyUI…").
    void install_progress(const std::string& mod_id, int percent,
                          const std::string& status);
    // name is the real display name resolved by the provider (e.g. "SkyUI"),
    // or empty when no provider info was available.
    void download_complete(const std::string& mod_id, bool success,
                           const std::string& archive_path,
                           const std::string& name = {});
    // Emitted as soon as the provider resolves the download's metadata - right
    // before the bytes start flowing (FetchStage fires on_download_meta).
    // display_name is the source-resolved mod/file name ("" when unknown);
    // archive_name is the real archive filename the download will be saved as.
    // The UI uses this to replace the placeholder row name immediately.
    void download_meta(const std::string& mod_id,
                       const std::string& archive_name,
                       const std::string& display_name);
    // installed_folder is the final mods/<folder> the install produced (empty
    // when nothing was installed, e.g. a metadata-only mod). The UI can add
    // just that one row instead of rescanning the whole mods dir.
    void install_complete(const std::string& mod_id, bool success,
                          const std::string& message,
                          const std::string& installed_folder = {});
    // The user canceled an interactive install stage (FOMOD wizard, overwrite
    // dialog). The download's state must be left untouched - this is NOT a
    // failure, and no Failed/Installed mark is applied.
    void install_canceled(const std::string& mod_id);
    void paused(const std::string& mod_id);
    void all_done();

private:
    struct PendingDownload {
        std::string id;
        engine::Mod mod;
        std::string mods_dir;
        std::string meta_dir;
    };

    // Route a new download to a free pool slot; if every slot is busy, park it
    // in pending_ and let on_fetch_finished drain the queue in order.
    void dispatch_fetch(PendingDownload&& pd);
    // Mark the slot busy, reset its pause flag, record the id->slot mapping and
    // dispatch run() onto the slot's thread.
    void start_fetch_on(FetchRunner* runner, PendingDownload&& pd);
    // A slot's transfer ended (any outcome): free it and start the next
    // queued download on it.
    void on_fetch_finished(const std::string& id);

    std::unique_ptr<engine::Pipeline> pipeline_;
    engine::PipelineContext ctx_;
    // Fetch pool. Each runner has its own pipeline + thread; downloads fan out
    // over these and the worker thread itself never blocks on a transfer.
    std::vector<std::unique_ptr<FetchRunner>> runners_;
    std::size_t next_runner_ = 0;
    // Downloads that arrived while every slot was busy, FIFO.
    std::deque<PendingDownload> pending_;
    // id -> slot currently fetching it. All accesses happen on the worker
    // thread (dispatch, pause, fetch_finished), so it needs no lock.
    std::unordered_map<std::string, FetchRunner*> running_;
    // ids of Nexus downloads currently in flight (source_type == "nexus").
    // Worker-thread-only, same as running_. Drives the one-at-a-time rule.
    std::unordered_set<std::string> nexus_in_flight_;
    // Settings mirror (Settings::nexus_queue_downloads), pushed by the owner.
    // Defaults ON so a pool used without an explicit push still queues.
    std::atomic_bool nexus_queue_downloads_{true};
};

// Wrapper to run PipelineWorker in a thread
class PipelineThread : public QObject {
    Q_OBJECT
public:
    explicit PipelineThread(QObject* parent = nullptr);
    ~PipelineThread();

    PipelineWorker* worker() const { return worker_; }

    void start();
    void stop();

signals:
    void operation_started();
    void operation_finished();

private:
    QThread* thread_ = nullptr;
    PipelineWorker* worker_ = nullptr;
};

}  // namespace ui
