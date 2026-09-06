#pragma once

#include "engine/source/modpub/provider.h"

#include <QObject>

#include <functional>

class QThread;

namespace ui {

// ModPub variant of SourceFetchWorker. Mirrors the LoversLab version
// (dedicated worker thread, queued functor, generation-tagged result) but
// carries the ModPub::ModInfoResult through the signal. Duplicating
// avoids templating SourceFetchWorker on the result type and the
// resulting Q_DECLARE_METATYPE explosion (only one metatype needs to be
// declared per concrete result type).
class ModPubFetchWorker : public QObject {
  Q_OBJECT
public:
  explicit ModPubFetchWorker(QObject *parent = nullptr);

  // Runs on the worker thread. Only ever invoked through
  // ModPubFetchThread::start(). Never throws; a fetch that fails simply
  // yields ModInfoResult::available=false.
  void run(std::function<engine::ModPubModInfoResult()> fetch,
           quint64 generation);

signals:
  void finished(engine::ModPubModInfoResult result, quint64 generation);
};

// Long-lived worker thread reusing the LootSortThread shape. start()
// queues one fetch; call it again for the next run. Owned by the panel,
// quit+wait on destruction so a destroyed panel never leaves a fetch
// running into a dead receiver.
class ModPubFetchThread : public QObject {
  Q_OBJECT
public:
  explicit ModPubFetchThread(QObject *parent = nullptr);
  ~ModPubFetchThread() override;

  ModPubFetchWorker *worker() const { return worker_; }

  void start(std::function<engine::ModPubModInfoResult()> fetch,
             quint64 generation);

private:
  QThread *thread_ = nullptr;
  ModPubFetchWorker *worker_ = nullptr;
};

} // namespace ui

Q_DECLARE_METATYPE(engine::ModPubModInfoResult)
