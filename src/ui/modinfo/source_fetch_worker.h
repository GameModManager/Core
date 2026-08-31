#pragma once

#include "engine/source/nexus_provider.h"

#include <QObject>

#include <functional>

class QThread;

namespace ui {

// Runs a caller-supplied Nexus mod-info fetch (network round-trip + JSON
// parse) on a dedicated worker thread and crosses the result back via a
// queued signal (P8.3 - SourceTab's Refresh used to block the main thread
// behind a WaitCursor). The fetch callable is delivered per-run through a
// queued functor (LootSortThread shape), so the worker owns no mutable state
// the UI thread could race with; `generation` tags which run a result belongs
// to, so a newer Refresh can drop an older in-flight result.
class SourceFetchWorker : public QObject {
    Q_OBJECT
public:
    explicit SourceFetchWorker(QObject* parent = nullptr);

    // Runs on the worker thread. Only ever invoked through
    // SourceFetchThread::start(). Never throws; a fetch that fails simply
    // yields ModInfoResult::available=false.
    void run(std::function<engine::ModInfoResult()> fetch, quint64 generation);

signals:
    void finished(engine::ModInfoResult result, quint64 generation);
};

// Long-lived worker thread reusing the LootSortThread shape. start() queues
// one fetch; call it again for the next run.
//
// Lifetime note: the thread is quit+waited on destruction, so destroying the
// owning SourceTab while a fetch is in flight blocks until the network call
// returns (bounded by the HTTP timeout) - the same trade the other worker
// threads accept; the worker is never left running into a dead receiver.
class SourceFetchThread : public QObject {
    Q_OBJECT
public:
    explicit SourceFetchThread(QObject* parent = nullptr);
    ~SourceFetchThread() override;

    SourceFetchWorker* worker() const { return worker_; }

    // Queue a fetch for the worker thread. The callable is copied into the
    // queued functor, so no shared state.
    void start(std::function<engine::ModInfoResult()> fetch, quint64 generation);

private:
    QThread* thread_ = nullptr;
    SourceFetchWorker* worker_ = nullptr;
};

}  // namespace ui

Q_DECLARE_METATYPE(engine::ModInfoResult)
