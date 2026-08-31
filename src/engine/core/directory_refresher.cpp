#include "engine/core/directory_refresher.h"

#include "engine/core/log/logger.h"

#include <QThread>

#include <algorithm>

namespace engine {

// =========================================================================
// PlaceholderWorker — scaffolding for coordinator-only compilation.
// Will be replaced by concrete workers when MainWindow migrates.
// =========================================================================

PlaceholderWorker::PlaceholderWorker(int target, QObject *parent)
    : QObject(parent), target_(target) {}

void PlaceholderWorker::run() {
  // Placeholder: emit finished immediately so the coordinator cleans up.
  emit finished(target_, true);
}

// =========================================================================
// DirectoryRefresher
// =========================================================================

DirectoryRefresher::DirectoryRefresher(QObject *parent) : QObject(parent) {}

DirectoryRefresher::~DirectoryRefresher() { cancel_all(); }

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void DirectoryRefresher::refresh(RefreshTargets targets) {
  if (targets.testFlag(RefreshTarget::All)) {
    targets = RefreshTargets(RefreshTarget::Mods) | RefreshTarget::Plugins |
              RefreshTarget::Conflicts | RefreshTarget::Saves |
              RefreshTarget::DataTab;
  }

  if (targets.testFlag(RefreshTarget::Mods))
    start_worker(RefreshTarget::Mods);
  if (targets.testFlag(RefreshTarget::Plugins))
    start_worker(RefreshTarget::Plugins);
  if (targets.testFlag(RefreshTarget::Conflicts))
    start_worker(RefreshTarget::Conflicts);
  if (targets.testFlag(RefreshTarget::Saves))
    start_worker(RefreshTarget::Saves);
  if (targets.testFlag(RefreshTarget::DataTab))
    start_worker(RefreshTarget::DataTab);
}

void DirectoryRefresher::cancel_all() {
  // Disconnect signals first so we don't re-enter on_worker_finished during
  // the loop.
  for (auto &entry : active_workers_) {
    if (entry.thread) {
      QObject::disconnect(entry.thread, nullptr, this, nullptr);
      if (entry.worker) {
        QObject::disconnect(entry.worker, nullptr, this, nullptr);
        entry.worker->deleteLater();
      }
      entry.thread->quit();
      entry.thread->wait();
      entry.thread->deleteLater();
    }
  }
  active_workers_.clear();
}

bool DirectoryRefresher::is_busy() const { return !active_workers_.empty(); }

int DirectoryRefresher::active_workers() const {
  return static_cast<int>(active_workers_.size());
}

// ---------------------------------------------------------------------------
// Worker lifecycle
// ---------------------------------------------------------------------------

void DirectoryRefresher::start_worker(RefreshTarget target) {
  auto *thread = new QThread(this);
  thread->setObjectName(
      QStringLiteral("gmm-dir-refresh-%1").arg(static_cast<int>(target)));

  auto *worker = new PlaceholderWorker(static_cast<int>(target), nullptr);
  worker->moveToThread(thread);

  // When the thread starts, run the worker.
  QObject::connect(thread, &QThread::started, worker, &PlaceholderWorker::run);

  // When the worker emits finished, relay it to the coordinator and stop the
  // thread.
  QObject::connect(worker, &PlaceholderWorker::finished, this,
                   [this, target](int /*t*/, bool success) {
                     on_worker_finished(target, success);
                   });

  // When the thread finishes, clean up the worker.
  QObject::connect(thread, &QThread::finished, worker, &QObject::deleteLater);
  QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);

  WorkerEntry entry;
  entry.thread = thread;
  entry.worker = worker;
  entry.target = target;
  active_workers_.push_back(entry);

  Logger::instance().debug("DirectoryRefresher: starting worker for target " +
                           std::to_string(static_cast<int>(target)));

  emit refresh_started(target);
  thread->start();
}

void DirectoryRefresher::on_worker_finished(RefreshTarget target,
                                            bool success) {
  // Remove the finished worker from the active list.
  auto it = std::remove_if(
      active_workers_.begin(), active_workers_.end(),
      [target](const WorkerEntry &e) { return e.target == target; });
  active_workers_.erase(it, active_workers_.end());

  Logger::instance().debug("DirectoryRefresher: worker finished for target " +
                           std::to_string(static_cast<int>(target)) +
                           " success=" + std::to_string(success ? 1 : 0));

  emit refresh_finished(target, success);

  if (active_workers_.empty()) {
    emit all_refreshes_finished();
  }
}

} // namespace engine
