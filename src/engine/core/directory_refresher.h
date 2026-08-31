#pragma once

// DirectoryRefresher - a coordinator/facade that manages all background scan
// workers from one place. Rather than creating ad-hoc QThread+worker pairs in
// MainWindow, callers request a refresh through this single entry point.
//
// The coordinator owns the lifecycle of each worker: it creates threads on
// demand, connects their completion signals to automatic cleanup, and emits
// aggregate progress/completion signals.  It does NOT move the existing worker
// classes; it is a coordinator that uses them, not a replacement.
//
// Thread safety: all public methods are expected to be called from the main
// (UI) thread.  The coordinator is a QObject - connect signals/slots normally.

#include <QObject>
#include <QString>

#include <vector>

class QThread;

namespace engine {

// Placeholder worker used by DirectoryRefresher for coordinator-only
// compilation.  Will be replaced by concrete workers (ModScanThread, etc.)
// when MainWindow migrates to DirectoryRefresher.
class PlaceholderWorker : public QObject {
  Q_OBJECT
public:
  explicit PlaceholderWorker(int target, QObject *parent = nullptr);

  // Called on the worker thread via queued connection.
  void run();

signals:
  void finished(int target, bool success);

private:
  int target_;
};

class DirectoryRefresher : public QObject {
  Q_OBJECT
public:
  explicit DirectoryRefresher(QObject *parent = nullptr);
  ~DirectoryRefresher() override;

  // -----------------------------------------------------------------------
  // What to refresh
  // -----------------------------------------------------------------------
  enum class RefreshTarget {
    Mods,      // scan mod directories
    Plugins,   // reload plugin database
    Conflicts, // scan for conflicts
    Saves,     // scan save files
    DataTab,   // rebuild data tab
    All        // everything
  };
  Q_DECLARE_FLAGS(RefreshTargets, RefreshTarget)

  // -----------------------------------------------------------------------
  // Public API
  // -----------------------------------------------------------------------

  // Trigger a refresh.  `targets` is a bitmask of RefreshTarget flags.
  // Non-blocking: workers run in the background and signal completion.
  // Calling refresh() while already busy is allowed - a second set of workers
  // is spawned; there is no queuing or deduplication.
  void refresh(RefreshTargets targets);

  // Cancel all running operations.  Emits refresh_finished(Cancelled) for
  // each active worker and all_refreshes_finished() once every worker has
  // stopped.
  void cancel_all();

  // True when at least one worker is active.
  [[nodiscard]] bool is_busy() const;

  // Number of workers currently running.
  [[nodiscard]] int active_workers() const;

signals:
  // Emitted when a specific target's worker starts.
  void refresh_started(RefreshTarget target);

  // Emitted when a specific target's worker finishes.
  // `success` is true when the worker completed without error; false on
  // cancellation or failure.
  void refresh_finished(RefreshTarget target, bool success);

  // Emitted once after the last active worker completes.
  void all_refreshes_finished();

  // Periodic progress relay from a worker.
  void progress(RefreshTarget target, int percentage,
                const QString &status_text);

private:
  // -----------------------------------------------------------------------
  // Internal bookkeeping
  // -----------------------------------------------------------------------
  struct WorkerEntry {
    QThread *thread = nullptr;
    QObject *worker = nullptr;
    RefreshTarget target;
  };

  std::vector<WorkerEntry> active_workers_;

  // Start a single worker for the given target.  Creates the appropriate
  // worker class, moves it to a new QThread, and connects its signals.
  void start_worker(RefreshTarget target);

  // Slot connected to every worker's "finished" signal.  Cleans up the
  // thread+worker pair and tracks completion state.
  void on_worker_finished(RefreshTarget target, bool success);
};

Q_DECLARE_OPERATORS_FOR_FLAGS(DirectoryRefresher::RefreshTargets)

} // namespace engine
