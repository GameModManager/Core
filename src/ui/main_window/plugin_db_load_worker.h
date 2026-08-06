#pragma once

#include "engine/plugins/plugin_database.h"

#include <QObject>

#include <filesystem>
#include <string>

class QThread;

namespace ui {

// Everything the plugin-DB load needs, copied off the UI thread at request
// time (THREADING.md §3.5: snapshots, never shared mutable state). The
// disable mechanism + game-native plugin list come from per-game knowledge,
// resolved on the main thread before dispatch.
struct PluginDbLoadRequest {
    std::filesystem::path game_dir;   // game install root (Data/ holds vanilla plugins)
    std::filesystem::path mods_dir;   // resolved mods_dir_path() (instance or game)
    std::filesystem::path meta_dir;   // resolved meta_dir_path(), empty in portable mode
    std::string disable_mechanism;    // sentinel filename marking a mod disabled
    std::string game_native;          // comma-separated vanilla plugins
};

// Runs the plugin-DB disk load (PluginDatabase::refresh -> parse headers ->
// scan plugin assets, then load_creation_club + sort_load_order) on the worker
// thread — the startup path's other directory walk / header parse, so it can
// overlap the mod scan instead of running after it. The result crosses back
// once via finished(); `generation` tags which load the result belongs to so a
// newer instance switch can drop an in-flight result. Profile load + mod-index
// generation + tab population stay on the main thread.
class PluginDbLoadWorker : public QObject {
    Q_OBJECT
public:
    explicit PluginDbLoadWorker(QObject* parent = nullptr);

    // Runs on the worker thread. Only ever invoked through PluginDbLoadThread::start().
    void run(PluginDbLoadRequest request, quint64 generation);

signals:
    void finished(engine::PluginDatabase db, quint64 generation);
};

// Long-lived worker thread reusing the ModScanThread/ConflictScanThread shape.
// start() queues one load; call it again for the next run.
class PluginDbLoadThread : public QObject {
    Q_OBJECT
public:
    explicit PluginDbLoadThread(QObject* parent = nullptr);
    ~PluginDbLoadThread() override;

    PluginDbLoadWorker* worker() const { return worker_; }

    // Queue a load for the worker thread. The request is copied into the
    // queued functor, so no shared state.
    void start(PluginDbLoadRequest request, quint64 generation);

private:
    QThread* thread_ = nullptr;
    PluginDbLoadWorker* worker_ = nullptr;
};

}  // namespace ui

Q_DECLARE_METATYPE(engine::PluginDatabase)
