#include "ui/main_window/conflict_scan_worker.h"

#include <QMetaObject>
#include <QThread>

#include <utility>

namespace ui {

ConflictScanWorker::ConflictScanWorker(QObject* parent) : QObject(parent) {}

void ConflictScanWorker::run(ConflictScanRequest request, quint64 generation) {
    engine::ConflictEngine engine;
    for (const auto& folder : request.invalidate)
        engine.invalidate_mod(folder, request.cache_path);

    ConflictScanResult result;
    result.conflict_reversed = request.conflict_reversed;
    result.stats = engine.compute(
        request.mods_dir, request.mod_infos, request.extensions_csv,
        request.ignored_csv, request.conflict_reversed, request.cache_path,
        request.extra_mods_dir, request.scan_dirs_csv);
    result.registry = engine.last_registry();
    emit finished(std::move(result), generation);
}

ConflictScanThread::ConflictScanThread(QObject* parent) : QObject(parent) {
    qRegisterMetaType<ui::ConflictScanResult>();
    thread_ = new QThread(this);
    thread_->setObjectName(QStringLiteral("gmm-conflict-scan"));
    worker_ = new ConflictScanWorker(nullptr);
    worker_->moveToThread(thread_);
    connect(thread_, &QThread::finished, worker_, &QObject::deleteLater);
    thread_->start();
}

ConflictScanThread::~ConflictScanThread() {
    thread_->quit();
    thread_->wait();
}

void ConflictScanThread::start(ConflictScanRequest request, quint64 generation) {
    ConflictScanWorker* worker = worker_;
    QMetaObject::invokeMethod(
        worker,
        [worker, req = std::move(request), gen = generation]() mutable {
            worker->run(std::move(req), gen);
        },
        Qt::QueuedConnection);
}

}  // namespace ui
