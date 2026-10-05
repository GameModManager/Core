#include "ui/modinfo/source_fetch_worker.h"

#include <QMetaObject>
#include <QThread>

#include <utility>

namespace ui {

SourceFetchWorker::SourceFetchWorker(QObject *parent) : QObject(parent) {}

void SourceFetchWorker::run(std::function<engine::Source::Nexus::ModInfoResult()> fetch,
                            quint64 generation) {
  engine::Source::Nexus::ModInfoResult result =
      fetch ? fetch() : engine::Source::Nexus::ModInfoResult{};
  emit finished(std::move(result), generation);
}

SourceFetchThread::SourceFetchThread(QObject *parent) : QObject(parent) {
  qRegisterMetaType<engine::Source::Nexus::ModInfoResult>();
  thread_ = new QThread(this);
  thread_->setObjectName(QStringLiteral("gmm-source-fetch"));
  worker_ = new SourceFetchWorker(nullptr);
  worker_->moveToThread(thread_);
  connect(thread_, &QThread::finished, worker_, &QObject::deleteLater);
  thread_->start();
}

SourceFetchThread::~SourceFetchThread() {
  thread_->quit();
  thread_->wait();
}

void SourceFetchThread::start(
    std::function<engine::Source::Nexus::ModInfoResult()> fetch, quint64 generation) {
  SourceFetchWorker *worker = worker_;
  QMetaObject::invokeMethod(
      worker,
      [worker, fetch = std::move(fetch), gen = generation]() mutable {
        worker->run(std::move(fetch), gen);
      },
      Qt::QueuedConnection);
}

}  // namespace ui
