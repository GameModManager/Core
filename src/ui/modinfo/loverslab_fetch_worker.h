#pragma once

#include "engine/source/loverslab_provider.h"

#include <QObject>

#include <functional>

class QThread;

namespace ui {

// LoversLab variant of SourceFetchWorker. Mirrors the Nexus version
// (dedicated worker thread, queued functor, generation-tagged result) but
// carries the LoversLab::ModInfoResult through the signal. Duplicating
// avoids templating SourceFetchWorker on the result type and the resulting
// Q_DECLARE_METATYPE explosion (only one metatype needs to be declared).
class LoversLabFetchWorker : public QObject {
  Q_OBJECT
public:
  explicit LoversLabFetchWorker(QObject *parent = nullptr);

  // Runs on the worker thread. Only ever invoked through
  // LoversLabFetchThread::start(). Never throws; a fetch that fails
  // simply yields ModInfoResult::available=false.
  void run(std::function<engine::LoversLabModInfoResult()> fetch,
           quint64 generation);

signals:
  void finished(engine::LoversLabModInfoResult result, quint64 generation);
};

// Long-lived worker thread reusing the LootSortThread shape. start()
// queues one fetch; call it again for the next run. Owned by the panel,
// quit+wait on destruction so a destroyed panel never leaves a fetch
// running into a dead receiver.
class LoversLabFetchThread : public QObject {
  Q_OBJECT
public:
  explicit LoversLabFetchThread(QObject *parent = nullptr);
  ~LoversLabFetchThread() override;

  LoversLabFetchWorker *worker() const { return worker_; }

  void start(std::function<engine::LoversLabModInfoResult()> fetch,
             quint64 generation);

private:
  QThread *thread_ = nullptr;
  LoversLabFetchWorker *worker_ = nullptr;
};

} // namespace ui

Q_DECLARE_METATYPE(engine::LoversLabModInfoResult)