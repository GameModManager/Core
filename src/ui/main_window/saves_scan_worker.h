#pragma once

// Background save scanning for the Saves tab. Mirrors LootSortThread: a
// long-lived moveToThread worker that runs one scan per request. Scanning +
// parsing every .ess is I/O and decompression work, so it never runs on the
// main thread (THREADING.md §0). The request is a value snapshot taken on the
// main thread (saves dir, extensions, the current plugin load order, mods
// dir); the worker copies the game's save files and computes per-save missing
// assets, then streams the result back as one entryReady signal per save so
// the UI can render them progressively (Workspace-0owv) instead of waiting
// for the full batch.

#include "engine/game/plugins/plugin_info.h"
#include "engine/game/saves/save_game.h"
#include "engine/game/saves/save_missing_assets.h"

#include <QObject>
#include <QString>
#include <QVector>

#include <filesystem>
#include <string>
#include <vector>

class QThread;

namespace ui {

// One parsed save plus the missing-assets result for the request's load order.
struct SavesScanResultEntry {
    engine::SaveGame save;
    std::vector<engine::SaveMissingAsset> missing;
};

struct SavesScanResult {
    QVector<SavesScanResultEntry> entries;  // newest first (scanner-sorted)
    std::filesystem::path saves_dir;
};

// Context for one scan. All value types copied into the queued functor, so the
// worker shares no mutable state with the UI thread.
struct SavesScanRequest {
    std::filesystem::path saves_dir;
    std::vector<std::string> extensions;
    std::string game_id;                       // tags parsed saves ("skyrimse", …)
    std::vector<engine::GamePlugin> plugins;   // current load order snapshot
    std::filesystem::path mods_dir;
    std::filesystem::path overwrite_dir;
};

class SavesScanWorker : public QObject {
    Q_OBJECT
public:
    explicit SavesScanWorker(QObject* parent = nullptr);

    // Runs on the worker thread. Only ever invoked through
    // SavesScanThread::start().
    void run(SavesScanRequest request);

signals:
    // One entry per successfully parsed save. Fired on the worker thread
    // (SavesScanThread::start) - the UI connects with Qt::QueuedConnection so
    // the slot runs on the main thread. `done` is 1-based, `total` is the
    // path-enumeration count (used to drive a progress bar; equals the count
    // of files that matched the extension filter, including ones that failed
    // to parse).
    void entryReady(SavesScanResultEntry entry, int done, int total);
    // Final signal: the scan completed (or had no work to do). `count` is the
    // number of entryReady signals actually fired. UI uses this to flip
    // scanning_=false and drain any coalesced pending_request_.
    void finished(int count);
};

// Long-lived worker thread reusing the PipelineThread/LootSortThread shape.
// start() queues one scan; call it again for the next refresh.
class SavesScanThread : public QObject {
    Q_OBJECT
public:
    explicit SavesScanThread(QObject* parent = nullptr);
    ~SavesScanThread() override;

    SavesScanWorker* worker() const { return worker_; }

    // Queue a scan for the worker thread.
    void start(SavesScanRequest request);

signals:
    void operation_finished();

private:
    QThread* thread_ = nullptr;
    SavesScanWorker* worker_ = nullptr;
};

}  // namespace ui

Q_DECLARE_METATYPE(ui::SavesScanResultEntry)
