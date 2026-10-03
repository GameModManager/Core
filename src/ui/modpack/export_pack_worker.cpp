#include "ui/modpack/export_pack_worker.h"

#include <QMetaObject>
#include <QThread>

#include <memory>
#include <utility>

namespace ui {

ExportPackWorker::ExportPackWorker(QObject *parent) : QObject(parent) {}

void ExportPackWorker::run(PackBuildRequest request,
                           std::shared_ptr<engine::gmmpack::PackCancel> cancel) {
  const engine::gmmpack::PackCancel *flag = cancel.get();
  PackBuildResult result;
  if (request.export_pack) {
    result.export_result = engine::gmmpack::create_gmmpack(
        request.snapshot, request.mods_dir, request.options, request.output_path);
    result.ok    = result.export_result.ok;
    result.error = result.export_result.error;
    // create_gmmpack is not cancellable, so a raised flag cannot have been
    // observed mid-write; clear it so the wizard does not report a cancelled
    // export it actually finished.
    cancel->clear();
  } else {
    result.pack = engine::gmmpack::build_gmmpack(request.snapshot, request.mods_dir,
                                                 request.options, flag);
    result.cancelled = flag->cancelled();
    // A cancelled build is a partial pack: never present it as a preview.
    result.ok = !result.cancelled;
  }
  emit finished(std::move(result));
}

ExportPackThread::ExportPackThread(QObject *parent) : QObject(parent) {
  qRegisterMetaType<ui::PackBuildResult>();
  // A sink exists before the first start() so a caller can poll it unconditionally.
  cancel_->progress = std::make_shared<engine::gmmpack::PackProgress>();
  thread_           = new QThread(this);
  thread_->setObjectName(QStringLiteral("gmm-export-pack"));
  worker_ = new ExportPackWorker(nullptr);
  worker_->moveToThread(thread_);
  connect(thread_, &QThread::finished, worker_, &QObject::deleteLater);
  thread_->start();
}

ExportPackThread::~ExportPackThread() {
  // Raise the flag first: an in-flight build notices it at its next file
  // boundary, so joining does not wait out a full hash pass.
  cancel();
  thread_->quit();
  thread_->wait();
}

void ExportPackThread::start(PackBuildRequest request) {
  cancel_->clear();
  // A fresh sink per run: the previous run's stage and counters are stale, and
  // a bar reading them would show progress that never happened.
  cancel_->progress        = std::make_shared<engine::gmmpack::PackProgress>();
  ExportPackWorker *worker = worker_;
  QMetaObject::invokeMethod(
      worker,
      [worker, req = std::move(request), flag = cancel_]() mutable {
        worker->run(std::move(req), flag);
      },
      Qt::QueuedConnection);
}

void ExportPackThread::cancel() {
  cancel_->raise();
}

}  // namespace ui
