#pragma once

#include "engine/detect/mod_scanner.h"
#include "engine/registry/game_knowledge.h"

#include <QObject>

#include <filesystem>
#include <string>
#include <vector>

class QThread;

namespace ui {

// Everything the instance/mod scan needs, copied off the UI thread at request
// time (THREADING.md §3.5: snapshots, never shared mutable state). The
// knowledge copy is populated once at plugin registration and read-only
// afterwards, so handing it to a worker is safe.
struct ModScanRequest {
    engine::GameKnowledge knowledge;  // per-game hooks, copied
    std::string game_id;
    std::filesystem::path game_dir;      // game install dir
    std::filesystem::path instance_root; // empty = portable (no-instance) mode
    std::filesystem::path mods_dir;      // resolved mods_dir_path() (instance or game)
    std::filesystem::path meta_dir;      // resolved meta_dir_path(), empty in portable mode
};

struct ModScanResult {
    std::vector<engine::ScannedMod> scanned;
};

// Runs the mods-dir scan (ModScanner::scan/scan_dir), the game-native plugin
// synthesis + stray-plugin scan, and the one-time MO2 meta import on the
// worker thread — the load path's directory walking. The result crosses back
// once via finished(); `generation` tags which scan the result belongs to so a
// newer refresh / instance switch can drop an older in-flight result.
class ModScanWorker : public QObject {
    Q_OBJECT
public:
    explicit ModScanWorker(QObject* parent = nullptr);

    // Runs on the worker thread. Only ever invoked through ModScanThread::start().
    void run(ModScanRequest request, quint64 generation);

signals:
    void finished(ModScanResult result, quint64 generation);
};

// Long-lived worker thread reusing the ConflictScanThread/LootSortThread shape.
// start() queues one scan; call it again for the next run. Scans serialize on
// this single thread, so the per-instance meta-import writes never interleave.
class ModScanThread : public QObject {
    Q_OBJECT
public:
    explicit ModScanThread(QObject* parent = nullptr);
    ~ModScanThread() override;

    ModScanWorker* worker() const { return worker_; }

    // Queue a scan for the worker thread. The request is copied into the
    // queued functor, so no shared state.
    void start(ModScanRequest request, quint64 generation);

private:
    QThread* thread_ = nullptr;
    ModScanWorker* worker_ = nullptr;
};

}  // namespace ui

Q_DECLARE_METATYPE(ui::ModScanResult)
