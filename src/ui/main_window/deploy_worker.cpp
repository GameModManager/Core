#include "ui/main_window/deploy_worker.h"

#include <QElapsedTimer>
#include <QMetaObject>
#include <QThread>

#include <utility>

namespace ui {

DeployWorker::DeployWorker(QObject* parent) : QObject(parent) {}

void DeployWorker::run(engine::LaunchPrepRequest req) {
    // Throttle progress to ~10 Hz; the final state (done == total) always
    // passes so the UI ends on the exact count. QElapsedTimer is reentrant and
    // elapsed() only reads its internal timestamp, so concurrent invocations
    // from the executor's worker threads are safe.
    QElapsedTimer throttle;
    throttle.start();
    engine::LaunchParams params = engine::prepare_launch_params(
        req, [this, &throttle](int done, int total) {
            if (throttle.elapsed() >= 100 || done >= total)
                emit progress(done, total);
        });
    // Only after the deploy is fully finished.
    emit prepared(std::move(params));
}

DeployThread::DeployThread(QObject* parent) : QObject(parent) {
    qRegisterMetaType<engine::LaunchParams>();
    qRegisterMetaType<engine::LaunchPrepRequest>();
    thread_ = new QThread(this);
    thread_->setObjectName(QStringLiteral("gmm-deploy"));
    worker_ = new DeployWorker(nullptr);
    worker_->moveToThread(thread_);
    connect(thread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(thread_, &QThread::finished, this, &DeployThread::operation_finished);
    thread_->start();
}

DeployThread::~DeployThread() {
    thread_->quit();
    thread_->wait();
}

void DeployThread::start(engine::LaunchPrepRequest req) {
    DeployWorker* worker = worker_;
    QMetaObject::invokeMethod(
        worker,
        [worker, req = std::move(req)]() mutable {
            worker->run(std::move(req));
        },
        Qt::QueuedConnection);
}

}  // namespace ui
