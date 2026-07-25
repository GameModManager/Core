#pragma once

#include "engine/pipeline/pipeline.h"

#include <QObject>
#include <QThread>
#include <memory>
#include <string>

namespace engine {
class Pipeline;
struct Mod;
}

namespace ui {

class PipelineWorker : public QObject {
    Q_OBJECT
public:
    explicit PipelineWorker(QObject* parent = nullptr);
    ~PipelineWorker();

    void set_pipeline(std::unique_ptr<engine::Pipeline> pipeline);
    void set_context(engine::PipelineContext ctx);

public slots:
    void install_mod(const std::string& id, const std::string& zip_path);
    void remove_mod(const std::string& id);

signals:
    void progress(const std::string& mod_id, int stage_index, const std::string& stage_name);
    void finished(const std::string& mod_id, bool success, const std::string& message);
    void all_done();

private:
    std::unique_ptr<engine::Pipeline> pipeline_;
    engine::PipelineContext ctx_;
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
