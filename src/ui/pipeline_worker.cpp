#include "ui/pipeline_worker.h"
#include "engine/pipeline/pipeline.h"
#include "engine/pipeline/fetch_stage.h"
#include "engine/pipeline/stage.h"
#include "engine/model/mod.h"
#include "engine/source/source_provider.h"
#include "engine/log/logger.h"

namespace ui {

// --- PipelineWorker ---

PipelineWorker::PipelineWorker(QObject* parent)
    : QObject(parent) {}

PipelineWorker::~PipelineWorker() = default;

void PipelineWorker::set_pipeline(std::unique_ptr<engine::Pipeline> pipeline) {
    pipeline_ = std::move(pipeline);
}

void PipelineWorker::set_context(engine::PipelineContext ctx) {
    ctx_ = std::move(ctx);

    // Build the fetch-only pipeline used by download_mod. Downloads are
    // decoupled from installs (MO2 model): this pipeline never extracts or
    // installs, it just produces the archive in the instance downloads dir.
    fetch_pipeline_ = std::make_unique<engine::Pipeline>();
    fetch_pipeline_->set_context(ctx_);
    fetch_pipeline_->set_flow_id("download");
    fetch_pipeline_->add_stage(std::make_unique<engine::FetchStage>());
}

void PipelineWorker::install_mod(const std::string& id, const std::string& zip_path,
                                  const std::string& source_type,
                                  const std::string& source_id, int file_id) {
    engine::Logger::instance().debug("Installing mod: " + id);

    if (!pipeline_) {
        emit install_complete(id, false, "No pipeline configured");
        return;
    }

    engine::Mod mod;
    mod.id = id;
    mod.name = id;
    mod.state = engine::ModState::Downloaded;
    mod.download_source_type = source_type;
    mod.download_source_id = source_id;
    mod.download_nxm.file_id = file_id;

    // Add the zip file to mod files
    engine::ModFile file;
    file.relative_path = zip_path;
    mod.files.push_back(file);

    bool success = pipeline_->run(mod);

    if (success) {
        engine::Logger::instance().debug("Mod installed: " + id);
        emit install_complete(id, true, "Success");
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

    if (!fetch_pipeline_) {
        emit download_complete(id, false, "");
        return;
    }

    // Reset the pause flag for this id and wire it into the fetch context.
    auto& flag = cancel_flags_[id];
    flag.store(false);
    fetch_pipeline_->ctx().should_abort = [&flag]() { return flag.load(); };
    fetch_pipeline_->ctx().download_paused = false;
    fetch_pipeline_->ctx().download_resume_from = 0;

    if (!mods_dir.empty())
        fetch_pipeline_->ctx().mods_dir = std::filesystem::path(mods_dir);
    if (!meta_dir.empty())
        fetch_pipeline_->ctx().meta_dir = std::filesystem::path(meta_dir);

    fetch_pipeline_->ctx().on_progress = [this, id](int64_t dl, int64_t total, double speed) {
        emit download_progress(id, dl, total, speed);
    };

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

    bool success = fetch_pipeline_->run(mod);

    // Clean up progress callback
    fetch_pipeline_->ctx().on_progress = nullptr;

    if (fetch_pipeline_->ctx().download_paused) {
        engine::Logger::instance().debug("Download paused: " + id);
        emit paused(id);
        return;
    }

    // FetchStage records the downloaded archive in mod.files[0].
    std::string archive_path;
    if (!mod.files.empty())
        archive_path = mod.files[0].relative_path;

    if (success) {
        engine::Logger::instance().debug("Download complete: " + id);
        emit download_complete(id, true, archive_path);
    } else {
        engine::Logger::instance().error("Download failed: " + id);
        emit download_complete(id, false, archive_path);
    }
}

void PipelineWorker::pause_download(const std::string& id) {
    auto it = cancel_flags_.find(id);
    if (it != cancel_flags_.end())
        it->second.store(true);
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
