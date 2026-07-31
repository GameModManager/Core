#include "ui/pipeline_worker.h"
#include "engine/pipeline/pipeline.h"
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
}

void PipelineWorker::install_mod(const std::string& id, const std::string& zip_path) {
    engine::Logger::instance().debug("Installing mod: " + id);

    if (!pipeline_) {
        emit finished(id, false, "No pipeline configured");
        return;
    }

    engine::Mod mod;
    mod.id = id;
    mod.name = id;
    mod.state = engine::ModState::Downloaded;

    // Add the zip file to mod files
    engine::ModFile file;
    file.relative_path = zip_path;
    mod.files.push_back(file);

    bool success = pipeline_->run(mod);

    if (success) {
        engine::Logger::instance().debug("Mod installed: " + id);
        emit finished(id, true, "Success");
    } else {
        engine::Logger::instance().error("Failed to install mod: " + id);
        emit finished(id, false, "Pipeline failed");
    }
}

void PipelineWorker::install_from_nxm(const engine::NxmLink& link,
                                       const std::string& game_id,
                                       const std::string& mods_dir,
                                       const std::string& meta_dir) {
    std::string mod_id = std::to_string(link.mod_id);

    engine::Logger::instance().debug("Downloading from NXM: mod=" + mod_id +
                                    " file=" + std::to_string(link.file_id));

    if (!pipeline_) {
        emit finished(mod_id, false, "No pipeline configured");
        return;
    }

    engine::Mod mod;
    mod.id = mod_id;
    mod.name = "Mod #" + mod_id;
    mod.state = engine::ModState::Downloaded;
    mod.download_source_type = "nexus";
    mod.download_source_id = mod_id;
    mod.download_nxm.file_id = link.file_id;
    mod.download_nxm.key = link.key;
    mod.download_nxm.expire = link.expire;
    mod.download_nxm.user_id = link.user_id;
    mod.download_nxm.nexus_domain = link.nexus_domain;

    // Update pipeline context with paths for this specific download
    if (!mods_dir.empty())
        pipeline_->ctx().mods_dir = std::filesystem::path(mods_dir);
    if (!meta_dir.empty())
        pipeline_->ctx().meta_dir = std::filesystem::path(meta_dir);

    // Set up progress callback that emits the Qt signal
    pipeline_->ctx().on_progress = [this, mod_id](int64_t dl, int64_t total, double speed) {
        emit download_progress(mod_id, dl, total, speed);
    };

    bool success = pipeline_->run(mod);

    // Clean up progress callback
    pipeline_->ctx().on_progress = nullptr;

    if (success) {
        engine::Logger::instance().debug("NXM download complete: " + mod.id);
        emit finished(mod_id, true, "Success");
    } else {
        engine::Logger::instance().error("NXM download failed: " + mod.id);
        emit finished(mod_id, false, "Pipeline failed");
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
