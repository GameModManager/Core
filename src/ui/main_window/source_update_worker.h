#pragma once

#include "engine/source/interface.h"

#include <QObject>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

class QThread;

namespace ui {

// One source's part of a sweep: which provider to ask and the credential it
// needs. The credential is read on the MAIN thread (the keyring is GUI-thread
// only) and handed over here, because the worker must not reach the keyring -
// and because a credential held in a snapshot is the only copy that exists off
// the GUI thread at all.
struct SourceUpdateEntry {
  std::string source_type;  // "nexus", "loverslab", "modpub"
  std::string credential;   // API key / cookie jar, as the provider reads it
};

// Everything one update sweep needs, copied off the UI thread at request time
// (THREADING.md §3.5: snapshots, never shared mutable state).
struct SourceUpdateRequest {
  std::filesystem::path mods_dir;  // instance mods_dir_path()
  std::string
      game_domain;  // game slug for API-backed sources, e.g. "skyrimspecialedition"
  std::vector<SourceUpdateEntry> entries;  // sources to check, in toolbar order
  // Sources deliberately not checked, echoed back into the result so the
  // status message can say why (no API key, game not on Nexus, ...).
  std::vector<std::string> skipped;
  int64_t now = 0;  // Unix seconds, stamped on every meta.ini
  // Ask every source about every mod, ignoring cached page state. One request
  // per mod, so this is the explicit-action path only.
  bool force = false;
};

// One source's outcome, in request order.
struct SourceUpdateOutcome {
  std::string source_type;
  engine::Source::SourceUpdateReport report;
};

// What the sweep produced: one outcome per checked source (in request order)
// plus the skip notes the caller echoed in.
struct SourceUpdateResult {
  std::vector<SourceUpdateOutcome> outcomes;
  std::vector<std::string> skip_notes;
};

// Runs every provider's check_updates() - the network I/O and the meta.ini
// writes - on the worker thread.
//
// The sweep is blocking and does real network I/O (libcurl), so it must never
// run on the GUI thread; and it WRITES to each mod's meta.ini, so it must not
// race the mod list's own read of those same files. One sweep at a time,
// re-entrancy guarded by the caller (ModListController::check_mod_updates).
class SourceUpdateWorker : public QObject {
  Q_OBJECT
public:
  explicit SourceUpdateWorker(QObject *parent = nullptr);

  // Runs on the worker thread. Only ever invoked through
  // SourceUpdateThread::start().
  void run(SourceUpdateRequest request, quint64 generation);

signals:
  void finished(ui::SourceUpdateResult result, quint64 generation);
};

// Long-lived worker thread reusing the ModScanThread/ConflictScanThread shape.
// start() queues one sweep; call it again for the next run.
class SourceUpdateThread : public QObject {
  Q_OBJECT
public:
  explicit SourceUpdateThread(QObject *parent = nullptr);
  ~SourceUpdateThread() override;

  SourceUpdateWorker *worker() const { return worker_; }

  // Queue a sweep for the worker thread. The request is copied into the queued
  // functor, so no shared state.
  void start(SourceUpdateRequest request, quint64 generation);

private:
  QThread *thread_            = nullptr;
  SourceUpdateWorker *worker_ = nullptr;
};

}  // namespace ui

Q_DECLARE_METATYPE(ui::SourceUpdateResult)
