#pragma once

#include "engine/core/instance/instance_utils.h"
#include "engine/deploy/launch/launcher.h"

#include <QObject>

class QThread;

namespace ui {

// Runs engine::prepare_launch_params — which deploys all enabled mods into
// <instance>/.gmm_staging on the parallel executor — on a dedicated worker
// thread (P8.4; the launch-time full deploy used to block the main thread).
//
// Contract (THREADING.md §3): the worker is created on the main thread and
// moved to its own QThread; `run` is only ever invoked through
// DeployThread::start() via a queued functor carrying a copy of the request,
// so the worker owns no mutable state the UI thread could race with.
// `progress` throttles link-operation progress (~10 Hz + final state);
// `prepared` is emitted only AFTER the deploy has fully finished — a consumer
// chains launch_game() on it and provably never launches before the staging
// tree is complete (the AGENTS.md session-end wipe invariant depends on that
// ordering). An instance switch mid-deploy yields a result the consumer drops
// (stale game_dir), never a torn launch.
class DeployWorker : public QObject {
    Q_OBJECT
public:
    explicit DeployWorker(QObject* parent = nullptr);

    void run(engine::LaunchPrepRequest req);

signals:
    void progress(int files_done, int files_total);
    void prepared(engine::LaunchParams params);
};

// Long-lived worker thread reusing the LootSortThread/SourceFetchThread
// shape. start() queues one launch-prep run; call it again for the next one.
// Lifetime note: the thread is quit+waited on destruction, so destroying the
// owning MainWindow while a deploy is in flight blocks until it finishes
// (bounded by the deploy itself) — the same trade the other worker threads
// accept; the worker is never left running into a dead receiver.
class DeployThread : public QObject {
    Q_OBJECT
public:
    explicit DeployThread(QObject* parent = nullptr);
    ~DeployThread() override;

    DeployWorker* worker() const { return worker_; }

    void start(engine::LaunchPrepRequest req);

signals:
    void operation_finished();

private:
    QThread* thread_ = nullptr;
    DeployWorker* worker_ = nullptr;
};

}  // namespace ui

Q_DECLARE_METATYPE(engine::LaunchParams)
Q_DECLARE_METATYPE(engine::LaunchPrepRequest)
