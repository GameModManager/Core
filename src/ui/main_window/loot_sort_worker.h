#pragma once

#include "engine/sort/loot/loot_sorter.h"

#include <QObject>
#include <QString>

class QThread;

Q_DECLARE_METATYPE(engine::LootResult)

namespace ui {

// Runs engine::run_loot_sort() on a background thread (LootSortThread). The
// request is delivered per-run through a queued functor, so the worker owns no
// mutable state the UI thread could race with. Progress stages are relayed via
// progress() (queued connection to the UI thread); the result arrives once via
// finished().
class LootSortWorker : public QObject {
    Q_OBJECT
public:
    explicit LootSortWorker(QObject* parent = nullptr);

    // Runs on the worker thread. Only ever invoked through LootSortThread::start().
    void run(engine::LootRequest request);

signals:
    void progress(int stage, const QString& message);
    void finished(engine::LootResult result);
};

// Long-lived worker thread reusing the PipelineThread shape. start() queues
// one sort; call it again for the next run.
class LootSortThread : public QObject {
    Q_OBJECT
public:
    explicit LootSortThread(QObject* parent = nullptr);
    ~LootSortThread() override;

    LootSortWorker* worker() const { return worker_; }

    // Queue a sort for the worker thread. The request is copied into the
    // queued functor, so no shared state.
    void start(engine::LootRequest request);

signals:
    void operation_finished();

private:
    QThread* thread_ = nullptr;
    LootSortWorker* worker_ = nullptr;
};

}  // namespace ui
