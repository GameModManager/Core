#pragma once

#include "engine/source/nexus/update_check.h"

#include <QObject>

#include <cstdint>
#include <filesystem>
#include <string>

class QThread;

namespace ui {

// Everything one Nexus update sweep needs, copied off the UI thread at request
// time (THREADING.md §3.5: snapshots, never shared mutable state). The API key
// is read on the MAIN thread from the keyring and handed over here, because the
// worker must not reach the keyring - and because a key held in a snapshot is
// the only copy that exists off the GUI thread at all.
struct NexusUpdateRequest {
  std::string nexus_domain;        // game slug, e.g. "skyrimspecialedition"
  std::string api_key;             // from Nexus::Auth::instance().get_api_key()
  std::filesystem::path mods_dir;  // instance mods_dir_path()
  int64_t now = 0;                 // Unix seconds, stamped on every meta.ini
};

// Runs UpdateCheck::run() - the bulk updated.json request, the per-mod
// files.json follow-ups, and the meta.ini writes - on the worker thread.
//
// The sweep is blocking and does real network I/O (libcurl), so it must never
// run on the GUI thread; and it WRITES to each mod's meta.ini, so it must not
// race the mod list's own read of those same files. One sweep at a time,
// re-entrancy guarded by the caller (ModListController::check_mod_updates).
class NexusUpdateWorker : public QObject {
  Q_OBJECT
public:
  explicit NexusUpdateWorker(QObject *parent = nullptr);

  // Runs on the worker thread. Only ever invoked through
  // NexusUpdateThread::start().
  void run(NexusUpdateRequest request, quint64 generation);

signals:
  void finished(engine::Source::Nexus::UpdateCheck::Report report, quint64 generation);
};

// Long-lived worker thread reusing the ModScanThread/ConflictScanThread shape.
// start() queues one sweep; call it again for the next run.
class NexusUpdateThread : public QObject {
  Q_OBJECT
public:
  explicit NexusUpdateThread(QObject *parent = nullptr);
  ~NexusUpdateThread() override;

  NexusUpdateWorker *worker() const { return worker_; }

  // Queue a sweep for the worker thread. The request is copied into the queued
  // functor, so no shared state.
  void start(NexusUpdateRequest request, quint64 generation);

private:
  QThread *thread_           = nullptr;
  NexusUpdateWorker *worker_ = nullptr;
};

}  // namespace ui

Q_DECLARE_METATYPE(engine::Source::Nexus::UpdateCheck::Report)
