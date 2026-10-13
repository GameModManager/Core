#include "ui/main_window/nexus_update_worker.h"

#include "engine/log/logger.h"

#include <QMetaObject>
#include <QThread>

namespace ui {

NexusUpdateWorker::NexusUpdateWorker(QObject *parent) : QObject(parent) {}

void NexusUpdateWorker::run(NexusUpdateRequest request, quint64 generation) {
  const auto report = engine::Source::Nexus::UpdateCheck::run(
      request.nexus_domain, request.api_key, request.mods_dir, request.now,
      request.force);

  // Counts only. The API key is never logged here or anywhere else on this
  // path - it is a bearer token for the user's Nexus account, and the request
  // it rides in reaches Network:: as a header the ring buffer redacts.
  engine::Logger::instance().debug(
      "Nexus update check: " + std::to_string(report.updates) + " updates from " +
      std::to_string(report.checked) + " queried / " + std::to_string(report.total) +
      " mods, " + std::to_string(report.requests) + " requests (generation " +
      std::to_string(generation) + ")");
  emit finished(report, generation);
}

NexusUpdateThread::NexusUpdateThread(QObject *parent) : QObject(parent) {
  qRegisterMetaType<engine::Source::Nexus::UpdateCheck::Report>(
      "engine::Source::Nexus::UpdateCheck::Report");
  thread_ = new QThread(this);
  thread_->setObjectName(QStringLiteral("gmm-nexus-update"));
  worker_ = new NexusUpdateWorker(nullptr);
  worker_->moveToThread(thread_);
  connect(thread_, &QThread::finished, worker_, &QObject::deleteLater);
  thread_->start();
}

NexusUpdateThread::~NexusUpdateThread() {
  thread_->quit();
  thread_->wait();
}

void NexusUpdateThread::start(NexusUpdateRequest request, quint64 generation) {
  NexusUpdateWorker *worker = worker_;
  QMetaObject::invokeMethod(
      worker,
      [worker, req = std::move(request), gen = generation]() mutable {
        worker->run(std::move(req), gen);
      },
      Qt::QueuedConnection);
}

}  // namespace ui