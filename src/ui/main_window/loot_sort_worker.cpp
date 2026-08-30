#include "ui/main_window/loot_sort_worker.h"

#include <QMetaObject>
#include <QThread>

#include <utility>

namespace ui {

LootSortWorker::LootSortWorker(QObject* parent) : QObject(parent) {}

void LootSortWorker::run(engine::Sorter::Loot::Request request) {
    engine::Sorter::Loot::Result result = engine::Sorter::Loot::run_sort(
        request, [this](int stage, const std::string& message) {
            emit progress(stage, QString::fromStdString(message));
        });
    emit finished(std::move(result));
}

LootSortThread::LootSortThread(QObject* parent) : QObject(parent) {
    qRegisterMetaType<engine::Sorter::Loot::Result>();
    thread_ = new QThread(this);
    thread_->setObjectName(QStringLiteral("gmm-loot-sort"));
    worker_ = new LootSortWorker(nullptr);
    worker_->moveToThread(thread_);
    connect(thread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(thread_, &QThread::finished, this, &LootSortThread::operation_finished);
    thread_->start();
}

LootSortThread::~LootSortThread() {
    thread_->quit();
    thread_->wait();
}

void LootSortThread::start(engine::Sorter::Loot::Request request) {
    LootSortWorker* worker = worker_;
    QMetaObject::invokeMethod(
        worker,
        [worker, req = std::move(request)]() mutable {
            worker->run(std::move(req));
        },
        Qt::QueuedConnection);
}

}  // namespace ui
