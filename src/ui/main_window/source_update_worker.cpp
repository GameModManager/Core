#include "ui/main_window/source_update_worker.h"

#include "engine/log/logger.h"
#include "engine/source/registry.h"

#include <QMetaObject>
#include <QThread>

namespace ui {

SourceUpdateWorker::SourceUpdateWorker(QObject *parent) : QObject(parent) {}

void SourceUpdateWorker::run(SourceUpdateRequest request, quint64 generation) {
  SourceUpdateResult result;
  result.skip_notes = std::move(request.skipped);

  for (auto &entry : request.entries) {
    const auto *provider =
        engine::Source::Registry::instance().provider_for(entry.source_type);
    if (!provider || !provider->can_check_updates())
      continue;

    engine::Source::UpdateSweepContext ctx;
    ctx.mods_dir    = request.mods_dir;
    ctx.game_domain = request.game_domain;
    ctx.credential  = entry.credential;
    ctx.now         = request.now;
    ctx.force       = request.force;

    auto report = provider->check_updates(ctx);

    // Counts only. Credentials are never logged here or anywhere else on this
    // path - they are bearer tokens for the user's accounts, and the requests
    // they ride in reach Network:: as headers the ring buffer redacts.
    engine::Logger::instance().debug(
        entry.source_type + " update check: " + std::to_string(report.updates) +
        " updates from " + std::to_string(report.total) + " mods (generation " +
        std::to_string(generation) + ")");

    SourceUpdateOutcome outcome;
    outcome.source_type = entry.source_type;
    outcome.report      = std::move(report);
    result.outcomes.push_back(std::move(outcome));
  }

  emit finished(std::move(result), generation);
}

SourceUpdateThread::SourceUpdateThread(QObject *parent) : QObject(parent) {
  qRegisterMetaType<ui::SourceUpdateResult>("ui::SourceUpdateResult");
  thread_ = new QThread(this);
  thread_->setObjectName(QStringLiteral("gmm-source-update"));
  worker_ = new SourceUpdateWorker(nullptr);
  worker_->moveToThread(thread_);
  connect(thread_, &QThread::finished, worker_, &QObject::deleteLater);
  thread_->start();
}

SourceUpdateThread::~SourceUpdateThread() {
  thread_->quit();
  thread_->wait();
}

void SourceUpdateThread::start(SourceUpdateRequest request, quint64 generation) {
  SourceUpdateWorker *worker = worker_;
  QMetaObject::invokeMethod(
      worker,
      [worker, req = std::move(request), gen = generation]() mutable {
        worker->run(std::move(req), gen);
      },
      Qt::QueuedConnection);
}

}  // namespace ui
