#include "ui/workers/pipeline_worker.h"
#include "engine/pipeline/pipeline.h"
#include "engine/pipeline/fetch_stage.h"
#include "engine/pipeline/stage.h"
#include "engine/model/mod.h"
#include "engine/source/source_provider.h"
#include "engine/log/logger.h"

#include <filesystem>

namespace ui {

// --- FetchRunner ---

FetchRunner::FetchRunner(QObject* parent)
    : QObject(parent) {
    // Downloads are decoupled from installs (MO2 model): this pipeline never
    // extracts or installs, it just produces the archive in the instance
    // downloads dir. Each runner owns its own copy so concurrent transfers
    // never share a PipelineContext (per-run mutable state - should_abort,
    // resume offset, progress callbacks).
    fetch_pipeline_ = std::make_unique<engine::Pipeline>();
    fetch_pipeline_->set_flow_id("download");
    fetch_pipeline_->add_stage(std::make_unique<engine::FetchStage>());

    thread_ = new QThread(this);
    thread_->setObjectName(QStringLiteral("gmm-download"));
    moveToThread(thread_);
    thread_->start();
}

FetchRunner::~FetchRunner() {
    stop();
}

void FetchRunner::stop() {
    if (thread_ && thread_->isRunning()) {
        // Cooperative: the in-flight transfer polls cancel_flag_ and aborts,
        // then run() returns and the queued quit is processed.
        cancel_flag_.store(true);
        thread_->quit();
        thread_->wait(3000);
    }
}

void FetchRunner::run(const std::string& id, engine::Mod mod,
                      const std::string& mods_dir, const std::string& meta_dir) {
    auto& ctx = fetch_pipeline_->ctx();

    // Reset the pause/resume fields for this run (the worker reset the cancel
    // flag before dispatching).
    ctx.should_abort = [this]() { return cancel_flag_.load(); };
    ctx.download_paused = false;
    ctx.download_resume_from = 0;

    if (!mods_dir.empty())
        ctx.mods_dir = std::filesystem::path(mods_dir);
    if (!meta_dir.empty())
        ctx.meta_dir = std::filesystem::path(meta_dir);

    ctx.on_progress = [this, id](int64_t dl, int64_t total, double speed) {
        emit download_progress(id, dl, total, speed);
    };

    // Fire the resolved name as soon as FetchStage knows it (before the bytes
    // flow) so the UI can drop its placeholder immediately. Cleared with the
    // progress callback after the run.
    ctx.on_download_meta =
        [this, id](const std::string& archive_name, const std::string& display_name) {
            emit download_meta(id, archive_name, display_name);
        };

    const bool success =
        fetch_pipeline_->run(mod) == engine::PipelineResult::Success;

    // Clean up progress callbacks
    ctx.on_progress = nullptr;
    ctx.on_download_meta = nullptr;

    if (ctx.download_paused) {
        engine::Logger::instance().debug("Download paused: " + id);
        emit paused(id);
        emit fetch_finished(id);
        return;
    }

    // FetchStage records the downloaded archive in mod.files[0].
    std::string archive_path;
    if (!mod.files.empty())
        archive_path = mod.files[0].relative_path;

    // Real display name only when the provider resolved one (FetchStage
    // overwrote the "Mod file <id>" placeholder); empty otherwise so the UI
    // keeps its own placeholder.
    std::string display_name =
        (mod.name == "Mod file " + id) ? std::string{} : mod.name;

    if (success) {
        engine::Logger::instance().debug("Download complete: " + id);
        emit download_complete(id, true, archive_path, display_name);
    } else {
        engine::Logger::instance().error("Download failed: " + id);
        emit download_complete(id, false, archive_path, display_name);
    }
    emit fetch_finished(id);
}

// --- PipelineWorker ---

PipelineWorker::PipelineWorker(QObject* parent)
    : QObject(parent) {
    // Build the download pool. Downloads are background work with no UI lock,
    // so a pool of transfer threads is safe; installs still serialize on this
    // worker (the UI locks for the duration of an install).
    for (int i = 0; i < kMaxConcurrentDownloads; ++i) {
        auto runner = std::make_unique<FetchRunner>();
        // Relay the pool slots' signals through this object so all existing
        // UI wiring keeps listening to a single, stable emitter.
        connect(runner.get(), &FetchRunner::download_progress,
                this, &PipelineWorker::download_progress);
        connect(runner.get(), &FetchRunner::download_meta,
                this, &PipelineWorker::download_meta);
        connect(runner.get(), &FetchRunner::download_complete,
                this, &PipelineWorker::download_complete);
        connect(runner.get(), &FetchRunner::paused,
                this, &PipelineWorker::paused);
        connect(runner.get(), &FetchRunner::fetch_finished,
                this, &PipelineWorker::on_fetch_finished);
        runners_.push_back(std::move(runner));
    }
}

PipelineWorker::~PipelineWorker() {
    // Stop every transfer thread. Bounded waits - an in-flight download aborts
    // cooperatively via its cancel flag, so this returns almost immediately.
    for (auto& runner : runners_) {
        runner->cancel_flag().store(true);
        runner->stop();
    }
}

void PipelineWorker::set_pipeline(std::unique_ptr<engine::Pipeline> pipeline) {
    pipeline_ = std::move(pipeline);
}

void PipelineWorker::set_context(engine::PipelineContext ctx) {
    ctx_ = std::move(ctx);
}

void PipelineWorker::install_mod(const std::string& id, const std::string& zip_path,
                                  const std::string& source_type,
                                  const std::string& source_id, int file_id,
                                  const std::string& name,
                                  const std::string& page_url) {
    engine::Logger::instance().debug("Installing mod: " + id);

    if (!pipeline_) {
        emit install_complete(id, false, "No pipeline configured");
        return;
    }

    engine::Mod mod;
    mod.id = id;
    mod.name = name.empty() ? id : name;
    mod.state = engine::ModState::Downloaded;
    mod.download_source_type = source_type;
    mod.download_source_id = source_id;
    mod.download_nxm.file_id = file_id;
    mod.download_page_url = page_url;

    // Add the zip file to mod files
    engine::ModFile file;
    file.relative_path = zip_path;
    mod.files.push_back(file);

    // Record the archive name (FetchStage used to set this; InstallStage's
    // meta.ini record reads it).
    mod.archive_filename = std::filesystem::path(zip_path).filename().string();

    // Route engine install-stage progress (extract/copy) to the UI. The
    // callback runs on this worker thread; the signal is auto-queued to the
    // main thread's progress dialog.
    pipeline_->ctx().on_stage_progress =
        [this, id](int percent, const std::string& status) {
            emit install_progress(id, percent, status);
        };

    auto result = pipeline_->run(mod);

    if (result == engine::PipelineResult::Success) {
        engine::Logger::instance().debug("Mod installed: " + id);
        emit install_complete(id, true, "Success",
                              pipeline_->ctx().installed_mod_folder);
    } else if (result == engine::PipelineResult::Canceled) {
        // User canceled an interactive stage (FOMOD wizard, overwrite dialog).
        // Not a failure: the download keeps whatever state it had.
        engine::Logger::instance().debug("Mod install canceled: " + id);
        emit install_canceled(id);
    } else {
        engine::Logger::instance().error("Failed to install mod: " + id);
        emit install_complete(id, false, "Pipeline failed");
    }
}

void PipelineWorker::download_mod(const std::string& id,
                                   const engine::NxmLink& link,
                                   const std::string& game_id,
                                   const std::string& mods_dir,
                                   const std::string& meta_dir) {
    (void)game_id;
    engine::Logger::instance().debug("Downloading mod file: " + id);

    engine::Mod mod;
    mod.id = id;
    mod.name = "Mod file " + id;
    mod.state = engine::ModState::Downloaded;
    mod.download_source_type = "nexus";
    mod.download_source_id = std::to_string(link.mod_id);
    mod.download_nxm.file_id = link.file_id;
    mod.download_nxm.key = link.key;
    mod.download_nxm.expire = link.expire;
    mod.download_nxm.user_id = link.user_id;
    mod.download_nxm.nexus_domain = link.nexus_domain;

    dispatch_fetch({id, std::move(mod), mods_dir, meta_dir});
}

void PipelineWorker::download_mod_url(const std::string& id,
                                      const std::string& url,
                                      const std::string& game_id,
                                      const std::string& mods_dir,
                                      const std::string& meta_dir) {
    (void)game_id;
    engine::Logger::instance().debug("Downloading URL: " + id);

    engine::Mod mod;
    mod.id = id;
    mod.name = "Mod file " + id;
    mod.state = engine::ModState::Downloaded;
    mod.download_source_type = "loverslab";
    mod.download_source_id = id;
    mod.download_url = url;

    dispatch_fetch({id, std::move(mod), mods_dir, meta_dir});
}

void PipelineWorker::dispatch_fetch(PendingDownload&& pd) {
    // Per-source queueing: when enabled, a Nexus download waits while any
    // other Nexus download is still in flight, even if a pool slot is free.
    // Other sources ignore the rule (a LoversLab download may run alongside).
    if (pd.mod.download_source_type == "nexus" && nexus_queue_downloads_.load() &&
        !nexus_in_flight_.empty()) {
        pending_.push_back(std::move(pd));
        return;
    }
    // Round-robin scan for a free slot (THREADING §5: transfers live on their
    // own threads, the UI is never blocked on network).
    for (std::size_t i = 0; i < runners_.size(); ++i) {
        auto* runner = runners_[(next_runner_ + i) % runners_.size()].get();
        if (runner->busy().load())
            continue;
        next_runner_ = (next_runner_ + i + 1) % runners_.size();
        start_fetch_on(runner, std::move(pd));
        return;
    }
    // Every slot is busy: park it. on_fetch_finished drains the queue in order
    // (FIFO - earlier downloads keep their place over later ones).
    pending_.push_back(std::move(pd));
}

void PipelineWorker::start_fetch_on(FetchRunner* runner, PendingDownload&& pd) {
    runner->busy().store(true);
    // Reset the cooperative-pause flag BEFORE dispatch so a stale flag from a
    // previous run on this slot can't abort the new download. A pause arriving
    // after this point still wins: it finds the id in running_ and the flag is
    // polled by the transfer before/while it starts.
    runner->cancel_flag().store(false);
    if (pd.mod.download_source_type == "nexus")
        nexus_in_flight_.insert(pd.id);
    running_[pd.id] = runner;

    const std::string id = pd.id;
    QMetaObject::invokeMethod(
        runner,
        [runner, id, mod = std::move(pd.mod),
         mods_dir = std::move(pd.mods_dir),
         meta_dir = std::move(pd.meta_dir)]() mutable {
            runner->run(id, std::move(mod), mods_dir, meta_dir);
        },
        Qt::QueuedConnection);
}

void PipelineWorker::on_fetch_finished(const std::string& id) {
    auto it = running_.find(id);
    if (it == running_.end())
        return;
    FetchRunner* runner = it->second;
    running_.erase(it);
    runner->busy().store(false);
    nexus_in_flight_.erase(id);

    // A slot freed up: hand it the next queued download, if any. Nexus
    // downloads respect the one-at-a-time rule - a queued Nexus download takes
    // the slot only when no other Nexus transfer is in flight. A non-Nexus
    // download may always take it (so the second pool slot never idles behind
    // a blocked Nexus front-runner).
    if (!pending_.empty()) {
        for (auto pit = pending_.begin(); pit != pending_.end(); ++pit) {
            if (pit->mod.download_source_type == "nexus" &&
                nexus_queue_downloads_.load() && !nexus_in_flight_.empty())
                continue;
            auto pd = std::move(*pit);
            pending_.erase(pit);
            start_fetch_on(runner, std::move(pd));
            return;
        }
    }
}

void PipelineWorker::pause_download(const std::string& id) {
    // In-flight: set the slot's cooperative cancel flag; the transfer's poll
    // loop aborts and keeps the partial file for a later resume.
    auto it = running_.find(id);
    if (it != running_.end()) {
        it->second->cancel_flag().store(true);
        return;
    }
    // Queued behind a busy pool but not started yet: drop it from the queue
    // and report the same terminal state. Nothing was fetched, so there is no
    // partial file to keep - the UI just shows Paused.
    for (auto pit = pending_.begin(); pit != pending_.end(); ++pit) {
        if (pit->id == id) {
            pending_.erase(pit);
            emit paused(id);
            return;
        }
    }
}

// --- PipelineThread ---

PipelineThread::PipelineThread(QObject* parent)
    : QObject(parent) {
    thread_ = new QThread(this);
    worker_ = new PipelineWorker();
    worker_->moveToThread(thread_);

    connect(thread_, &QThread::finished, worker_, &QObject::deleteLater);
}

PipelineThread::~PipelineThread() {
    stop();
}

void PipelineThread::start() {
    if (!thread_->isRunning()) {
        thread_->start();
    }
}

void PipelineThread::stop() {
    if (thread_->isRunning()) {
        thread_->quit();
        thread_->wait(3000);
    }
}

}  // namespace ui
