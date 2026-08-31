#pragma once

#include "engine/index/conflict_engine.h"

#include <QObject>

#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class QThread;

namespace ui {

// Everything ConflictEngine::compute() needs, copied off the UI thread at
// request time (THREADING.md §3.6: snapshot, never share mutable state).
// `invalidate` lists mods whose cached file list must be dropped before
// scanning - a hidden-file rename inside a subdir does not change the mod
// folder's quick token, so the stale cached list would otherwise win.
struct ConflictScanRequest {
    std::filesystem::path mods_dir;
    std::filesystem::path extra_mods_dir;
    std::filesystem::path cache_path;
    std::vector<engine::ConflictEngine::ModInfo> mod_infos;
    std::string extensions_csv;
    std::string ignored_csv;
    std::string scan_dirs_csv;
    bool conflict_reversed = false;
    std::unordered_set<std::string> invalidate;
};

struct ConflictScanResult {
    std::unordered_map<std::string, engine::ConflictStats> stats;
    engine::PathRegistry registry;
    bool conflict_reversed = false;
};

// Runs engine::ConflictEngine::compute() on the worker thread. The request is
// delivered per-run through a queued functor, so the worker owns no mutable
// state the UI thread could race with. The result crosses back once via
// finished(); `generation` tags which scan the result belongs to, so a newer
// scan's arrival can drop an older in-flight result.
class ConflictScanWorker : public QObject {
    Q_OBJECT
public:
    explicit ConflictScanWorker(QObject* parent = nullptr);

    // Runs on the worker thread. Only ever invoked through ConflictScanThread::start().
    void run(ConflictScanRequest request, quint64 generation);

signals:
    void finished(ConflictScanResult result, quint64 generation);
};

// Long-lived worker thread reusing the LootSortThread shape. start() queues
// one scan; call it again for the next run.
class ConflictScanThread : public QObject {
    Q_OBJECT
public:
    explicit ConflictScanThread(QObject* parent = nullptr);
    ~ConflictScanThread() override;

    ConflictScanWorker* worker() const { return worker_; }

    // Queue a scan for the worker thread. The request is copied into the
    // queued functor, so no shared state.
    void start(ConflictScanRequest request, quint64 generation);

private:
    QThread* thread_ = nullptr;
    ConflictScanWorker* worker_ = nullptr;
};

}  // namespace ui

Q_DECLARE_METATYPE(ui::ConflictScanResult)
