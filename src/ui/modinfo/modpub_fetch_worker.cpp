#include "ui/modinfo/modpub_fetch_worker.h"

#include <QMetaObject>
#include <QThread>

#include <utility>

namespace ui {

ModPubFetchWorker::ModPubFetchWorker(QObject *parent) : QObject(parent) {}

void ModPubFetchWorker::run(
    std::function<engine::ModPubModInfoResult()> fetch, quint64 generation) {
  engine::ModPubModInfoResult result =
      fetch ? fetch() : engine::ModPubModInfoResult{};
  emit finished(std::move(result), generation);
}

ModPubFetchThread::ModPubFetchThread(QObject *parent) : QObject(parent) {
  qRegisterMetaType<engine::ModPubModInfoResult>();
  thread_ = new QThread(this);
  thread_->setObjectName(QStringLiteral("gmm-modpub-fetch"));
  worker_ = new ModPubFetchWorker(nullptr);
  worker_->moveToThread(thread_);
  connect(thread_, &QThread::finished, worker_, &QObject::deleteLater);
  thread_->start();
}

ModPubFetchThread::~ModPubFetchThread() {
  thread_->quit();
  thread_->wait();
}

void ModPubFetchThread::start(
    std::function<engine::ModPubModInfoResult()> fetch, quint64 generation) {
  ModPubFetchWorker *worker = worker_;
  QMetaObject::invokeMethod(
      worker,
      [worker, fetch = std::move(fetch), gen = generation]() mutable {
        worker->run(std::move(fetch), gen);
      },
      Qt::QueuedConnection);
}

} // namespace ui
