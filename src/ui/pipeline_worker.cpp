#include "ui/pipeline_worker.h"
#include "engine/pipeline/pipeline.h"
#include "engine/pipeline/stage.h"
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
    engine::Logger::instance().info("Installing mod: " + id);

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
        engine::Logger::instance().info("Mod installed: " + id);
        emit finished(id, true, "Success");
    } else {
        engine::Logger::instance().error("Failed to install mod: " + id);
        emit finished(id, false, "Pipeline failed");
    }
}

void PipelineWorker::remove_mod(const std::string& id) {
    engine::Logger::instance().info("Removing mod: " + id);
    // TODO: implement removal logic
    emit finished(id, true, "Removed");
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
