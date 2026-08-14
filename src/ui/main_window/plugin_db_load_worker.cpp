#include "ui/main_window/plugin_db_load_worker.h"

#include "engine/core/log/logger.h"

#include <QMetaObject>
#include <QThread>

namespace ui {

PluginDbLoadWorker::PluginDbLoadWorker(QObject* parent) : QObject(parent) {}

void PluginDbLoadWorker::run(PluginDbLoadRequest request, quint64 generation) {
    engine::PluginDatabase db;
    db.refresh(request.game_dir, request.mods_dir, request.meta_dir,
               request.disable_mechanism, request.game_native);
    db.load_creation_club(request.game_dir);
    db.sort_load_order();

    engine::Logger::instance().debug(
        "Plugin DB preload: " + std::to_string(db.plugins().size()) +
        " plugins (generation " + std::to_string(generation) + ")");
    emit finished(std::move(db), generation);
}

PluginDbLoadThread::PluginDbLoadThread(QObject* parent) : QObject(parent) {
    qRegisterMetaType<engine::PluginDatabase>();
    thread_ = new QThread(this);
    thread_->setObjectName(QStringLiteral("gmm-plugin-db"));
    worker_ = new PluginDbLoadWorker(nullptr);
    worker_->moveToThread(thread_);
    connect(thread_, &QThread::finished, worker_, &QObject::deleteLater);
    thread_->start();
}

PluginDbLoadThread::~PluginDbLoadThread() {
    thread_->quit();
    thread_->wait();
}

void PluginDbLoadThread::start(PluginDbLoadRequest request, quint64 generation) {
    PluginDbLoadWorker* worker = worker_;
    QMetaObject::invokeMethod(
        worker,
        [worker, req = std::move(request), gen = generation]() mutable {
            worker->run(std::move(req), gen);
        },
        Qt::QueuedConnection);
}

}  // namespace ui
