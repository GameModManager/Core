#pragma once

// Background row-building for the Data tab's merged file tree. The full
// population of a large instance (tens of thousands of registry entries) does
// per-file stat + path-resolution work, so it never runs on the main thread
// (THREADING.md §0). Mirrors ConflictScanThread/SavesScanThread: a long-lived
// moveToThread worker that builds one row set per request. The request is a
// value snapshot taken on the main thread (registry, display names, deploy
// layout); the worker returns the pre-sorted DataTabRow vector via a queued
// signal, and the main thread only touches QTreeWidget items.
//
// The pure-compute helpers (build_data_row and friends) are declared here so
// DataTab::apply_mod() can build single rows on the main thread too - the
// incremental install path shares exactly the same row semantics as the
// background full build.

#include "engine/deploy/root_override.h"

#include <QMetaType>
#include <QObject>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

class QThread;

namespace ui {

// Pseudo mod id for game-native root files (skse64_loader.exe, ...). Never a
// real mod folder, so it must fail the "managed mod" / hide gates.
constexpr const char* kGameRootNativeId = "__game_root__";

// One row of the merged file tree (display path -> winner/size/providers).
struct DataTabRow {
    QString path;          // display path (hidden suffix stripped)
    QString vfs_path;      // game-dir-relative merged path (Add as Executable)
    QString real_path;     // on-disk path of the winning copy
    QString origin_id;     // winner mod id
    QString source;
    qint64 size = -1;
    int providers = 0;
    bool hidden = false;   // file carries .gmmhidden / .mohidden
    QStringList all_sources;
    QStringList provider_paths;
    QStringList provider_ids;

    // Row identity for the Data tab's incremental delta: when a freshly built
    // row equals the previously applied one, the tree item is left untouched.
    bool operator==(const DataTabRow& o) const {
        return path == o.path && vfs_path == o.vfs_path &&
               real_path == o.real_path && origin_id == o.origin_id &&
               source == o.source && size == o.size && providers == o.providers &&
               hidden == o.hidden && all_sources == o.all_sources &&
               provider_paths == o.provider_paths && provider_ids == o.provider_ids;
    }
    bool operator!=(const DataTabRow& o) const { return !(*this == o); }
};

// Context for one population pass. All value types are copied into the queued
// functor, so the worker shares no mutable state with the UI thread.
struct DataTabBuildRequest {
    std::unordered_map<std::string, std::vector<std::pair<std::string, int>>> registry;
    std::unordered_map<std::string, QString> display_names;  // mod id -> display name
    std::unordered_set<std::string> root_override_mods;      // [General] rootOverride ids
    bool conflict_reversed = false;
    bool root_view = false;   // build the Root view (root-override + game-native) rows
    std::filesystem::path mods_dir;
    std::filesystem::path game_mods_dir;
    std::filesystem::path game_root_dir;   // game install root (Root view native walk)
    std::string mods_subpath;
    std::string deploy_prefix;
    bool deploy_include_mod_id = false;
};

struct DataTabBuildResult {
    std::vector<DataTabRow> rows;  // dirs-first sorted (insert order == final order)
};

// Pure compute: turn the registry into the full row set for the requested view,
// dirs-first sorted so the main thread's chunked insert needs no re-sort.
// Defined in data_tab_build_worker.cpp.
std::vector<DataTabRow> build_data_tab_rows(const DataTabBuildRequest& request);

// Build one row from a registry entry (key_path -> (mod_id, priority)
// providers). key_path is the mod-folder-relative path the on-disk files live
// at; display_path is the data-view-relative path the row is shown under
// (classify_registry_path strips a leading deploy-prefix segment for
// root-override mods' data content). space / deploy_prefix /
// deploy_include_mod_id / root_override_mods classify the row's deploy layout
// so vfs_path (the Add-as-Executable path) matches the real staging target
// exactly, never the data-view-relative display path. Shared by the background
// full build and the incremental install path.
DataTabRow build_data_row(const std::string& key_path,
                          const std::string& display_path,
                          const std::vector<std::pair<std::string, int>>& owners,
                          const std::unordered_map<std::string, QString>& display_names,
                          bool conflict_reversed,
                          const std::filesystem::path& mods_dir,
                          const std::filesystem::path& game_mods_dir,
                          engine::DeploySpace space,
                          const std::string& deploy_prefix,
                          bool deploy_include_mod_id,
                          const std::unordered_set<std::string>& root_override_mods);

// Runs build_data_tab_rows() on the worker thread. Only ever invoked through
// DataTabBuildThread::start().
class DataTabBuildWorker : public QObject {
    Q_OBJECT
public:
    explicit DataTabBuildWorker(QObject* parent = nullptr);

    void run(DataTabBuildRequest request, quint64 generation);

signals:
    void finished(DataTabBuildResult result, quint64 generation);
};

// Long-lived worker thread reusing the ConflictScanThread shape. start() queues
// one build; call it again for the next run.
class DataTabBuildThread : public QObject {
    Q_OBJECT
public:
    explicit DataTabBuildThread(QObject* parent = nullptr);
    ~DataTabBuildThread() override;

    DataTabBuildWorker* worker() const { return worker_; }

    // Queue a build for the worker thread.
    void start(DataTabBuildRequest request, quint64 generation);

private:
    QThread* thread_ = nullptr;
    DataTabBuildWorker* worker_ = nullptr;
};

}  // namespace ui

Q_DECLARE_METATYPE(ui::DataTabBuildResult)
