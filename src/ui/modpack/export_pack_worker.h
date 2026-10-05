#pragma once

#include <QObject>

#include <filesystem>
#include <memory>

#include "engine/instance/instance_snapshot.h"
#include "engine/modpack/gmmpack/packer.h"

class QThread;

namespace ui {

// Everything the packer needs for one run, copied off the UI thread at request
// time (THREADING.md §3.6 shape: snapshot, never share mutable state). The
// worker never reads a widget - including/excluding a mod or editing the pack
// name is applied to the snapshot and options on the UI thread first.
struct PackBuildRequest {
  engine::InstanceSnapshot snapshot;
  std::filesystem::path mods_dir;
  engine::gmmpack::PackOptions options;
  // Write the archive instead of only assembling it in memory. A cancelled
  // run is only possible for the in-memory build (see create_gmmpack).
  bool export_pack = false;
  std::filesystem::path output_path;
};

struct PackBuildResult {
  bool ok        = false;
  bool cancelled = false;
  std::string error;
  engine::gmmpack::Gmmpack pack;
  engine::gmmpack::PackResult export_result;
};

// Runs build_gmmpack() (or create_gmmpack()) on the worker thread. Both walk
// every exported mod's folder and hash every bundled byte, so the cost scales
// with the instance, not with the dialog - this is what keeps the export
// wizard's page transitions off the GUI thread. The build publishes real counts
// into the PackCancel's progress sink, which the wizard polls for its bar.
class ExportPackWorker : public QObject {
  Q_OBJECT
public:
  explicit ExportPackWorker(QObject *parent = nullptr);

  // Runs on the worker thread. Only ever invoked through
  // ExportPackThread::start().
  void run(PackBuildRequest request,
           std::shared_ptr<engine::gmmpack::PackCancel> cancel);

signals:
  void finished(PackBuildResult result);
};

// Long-lived worker thread reusing the ConflictScanThread shape. start() queues
// one run; call it again for the next. cancel() raises the in-flight run's
// flag, which the packer checks per file, and the destructor joins the thread -
// so a closed wizard cannot leave work running against itself.
class ExportPackThread : public QObject {
  Q_OBJECT
public:
  explicit ExportPackThread(QObject *parent = nullptr);
  ~ExportPackThread() override;

  ExportPackWorker *worker() const { return worker_; }

  // Queue a run for the worker thread and clear the cancel flag for it.
  void start(PackBuildRequest request);

  // Ask the in-flight run to abandon its work at the next file boundary. Safe
  // to call when nothing is running.
  void cancel();

  // True while cancel() has been raised and the run has not acknowledged it.
  [[nodiscard]] bool cancelled() const { return cancel_->cancelled(); }

  // Live progress of the in-flight run, for the GUI thread to poll. A fresh sink
  // per start(), because the previous run's stage and counters are stale. Never
  // null: before the first start() it reports a fresh sink with nothing done.
  [[nodiscard]] std::shared_ptr<engine::gmmpack::PackProgress> progress() const {
    return cancel_->progress;
  }

private:
  QThread *thread_          = nullptr;
  ExportPackWorker *worker_ = nullptr;
  // Shared with the in-flight request, so the flag and the progress sink outlive
  // the wizard even if the thread is still winding down while the dialog is
  // destroyed.
  std::shared_ptr<engine::gmmpack::PackCancel> cancel_ =
      std::make_shared<engine::gmmpack::PackCancel>();
};

}  // namespace ui

Q_DECLARE_METATYPE(ui::PackBuildResult)