#include "ui/modinfo/loverslab_fetch_worker.h"

#include <QMetaObject>
#include <QThread>

#include <utility>

namespace ui {

LoversLabFetchWorker::LoversLabFetchWorker(QObject *parent) : QObject(parent) {}

void LoversLabFetchWorker::run(
    std::function<engine::LoversLabModInfoResult()> fetch, quint64 generation) {
  engine::LoversLabModInfoResult result =
      fetch ? fetch() : engine::LoversLabModInfoResult{};
  emit finished(std::move(result), generation);
}

LoversLabFetchThread::LoversLabFetchThread(QObject *parent) : QObject(parent) {
  qRegisterMetaType<engine::LoversLabModInfoResult>();
  thread_ = new QThread(this);
  thread_->setObjectName(QStringLiteral("gmm-loverslab-fetch"));
  worker_ = new LoversLabFetchWorker(nullptr);
  worker_->moveToThread(thread_);
  connect(thread_, &QThread::finished, worker_, &QObject::deleteLater);
  thread_->start();
}

LoversLabFetchThread::~LoversLabFetchThread() {
  thread_->quit();
  thread_->wait();
}

void LoversLabFetchThread::start(
    std::function<engine::LoversLabModInfoResult()> fetch, quint64 generation) {
  LoversLabFetchWorker *worker = worker_;
  QMetaObject::invokeMethod(
      worker,
      [worker, fetch = std::move(fetch), gen = generation]() mutable {
        worker->run(std::move(fetch), gen);
      },
      Qt::QueuedConnection);
}

} // namespace ui