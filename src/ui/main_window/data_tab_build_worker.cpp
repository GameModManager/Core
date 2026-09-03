#include "ui/main_window/data_tab_build_worker.h"

#include "engine/deploy/deploy_utils.h"
#include "engine/core/util/fs_utils.h"
#include "engine/parallel/parallel.h"
#include "ui/widgets/mod_list_model.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QSet>
#include <QThread>

#include <algorithm>
#include <cctype>

namespace ui {

// --- Pure row-building compute. Runs on the worker thread (full build) or on
// the main thread (incremental install path) - never touches widgets.

// On-disk path of a provider's copy: instance mods dir first, then the
// game-native mods dir fallback. The registry key is CI-normalized (directory
// components lowercased), but the mod keeps its on-disk casing - so when the
// exact-case lookup misses, resolve case-insensitively through
// resolve_deploy_target_ci to find the real file (exact exists() is the fast
// path; the CI walk only runs for dual-case trees).
static std::filesystem::path resolve_mod_file(const std::string& mod_id,
                                              const std::string& rel_path,
                                              const std::filesystem::path& mods_dir,
                                              const std::filesystem::path& game_mods_dir) {
    std::error_code ec;
    auto candidate = mods_dir / mod_id / rel_path;
    if (std::filesystem::exists(candidate, ec)) return candidate;
    candidate = engine::resolve_deploy_target_ci(mods_dir / mod_id / rel_path);
    if (std::filesystem::exists(candidate, ec)) return candidate;
    if (!game_mods_dir.empty()) {
        ec.clear();
        candidate = game_mods_dir / mod_id / rel_path;
        if (std::filesystem::exists(candidate, ec)) return candidate;
        candidate = engine::resolve_deploy_target_ci(game_mods_dir / mod_id / rel_path);
        if (std::filesystem::exists(candidate, ec)) return candidate;
    }
    return {};
}

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
                          const std::unordered_set<std::string>& root_override_mods) {
    DataTabRow row;
    row.path = QString::fromStdString(display_path).replace('\\', '/');

    // Winner = the provider that actually takes effect
    auto winner = conflict_reversed
        ? std::min_element(owners.begin(), owners.end(),
                           [](const auto& a, const auto& b) { return a.second < b.second; })
        : std::max_element(owners.begin(), owners.end(),
                           [](const auto& a, const auto& b) { return a.second < b.second; });

    QString winner_id = QString::fromStdString(winner->first);
    row.origin_id = winner_id;
    if (winner_id == QLatin1String(kOverwriteModId)) {
        row.source = QCoreApplication::translate("DataTab", "Overwrite");
    } else if (winner_id == QLatin1String(kMergedModId)) {
        row.source = QCoreApplication::translate("DataTab", "MERGED");
    } else {
        auto it = display_names.find(winner->first);
        row.source = it != display_names.end() ? it->second : winner_id;
    }

    // Hidden files (.gmmhidden here, .mohidden in MO2-imported instances)
    // display under their base name. A visible file with the same base name
    // wins the display slot; otherwise the hidden copy is shown dimmed (the
    // name survives so it can be un-hidden from the tab).
    if (engine::is_hidden_file(std::filesystem::path(display_path))) {
        row.hidden = true;
        const auto gmm = std::string(engine::kGmmHiddenSuffix);
        const auto mo2 = std::string(engine::kMo2HiddenSuffix);
        if (row.path.endsWith(QString::fromStdString(gmm)))
            row.path.chop(static_cast<int>(gmm.size()));
        else if (row.path.endsWith(QString::fromStdString(mo2)))
            row.path.chop(static_cast<int>(mo2.size()));
    }

    row.providers = static_cast<int>(owners.size());
    for (const auto& [owner, _] : owners) {
        auto it = display_names.find(owner);
        row.all_sources << (it != display_names.end() ? it->second
                                                      : QString::fromStdString(owner));
        row.provider_ids << QString::fromStdString(owner);
        row.provider_paths << QString::fromStdString(
            resolve_mod_file(owner, key_path, mods_dir, game_mods_dir).string());
    }

    // Size and real path of the winning copy (hidden suffix intact)
    std::error_code ec;
    const auto winner_real = resolve_mod_file(winner->first, key_path, mods_dir, game_mods_dir);
    if (!winner_real.empty()) {
        row.real_path = QString::fromStdString(winner_real.string());
        auto sz = std::filesystem::file_size(winner_real, ec);
        if (!ec) row.size = static_cast<qint64>(sz);
    }

    // Merged (game-dir-relative) path the launch overlay resolves: for Data
    // rows the deploy prefix, then the mod-folder segment for
    // include-mod-id games (skipped for root-override mods, whose Data content
    // deploys straight under the prefix), then the display path. Root rows
    // deploy at the game root, so their display path already is the merged one.
    QString merged;
    if (space == engine::DeploySpace::Data) {
        if (!deploy_prefix.empty())
            merged = QString::fromStdString(deploy_prefix);
        if (deploy_include_mod_id &&
            !root_override_mods.count(row.origin_id.toStdString())) {
            if (!merged.isEmpty()) merged += '/';
            merged += row.origin_id;
        }
    }
    row.vfs_path = merged.isEmpty() ? row.path : merged + "/" + row.path;
    return row;
}

// True if `name` is a game-root folder the Root view must not descend into:
// the game's own data dir (shown by the merged Data view), the instance mods
// dir, overwrite, and the deploy staging dir. Compared case-insensitively
// (Isaac's real folder is "Mods" while its mods_subpath is "mods").
static bool is_reserved_root_dir(const std::string& name, const std::string& mods_subpath) {
    auto lname = name;
    for (char& c : lname) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::unordered_set<std::string> reserved = {
        "overwrite", "merged", ".merged", ".gmm_staging"};
    auto lsub = mods_subpath;
    for (char& c : lsub) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (!lsub.empty()) reserved.insert(std::move(lsub));
    else reserved.insert("data");
    return reserved.count(lname) > 0;
}

// One row for a game-native file sitting in the game root (skse64_loader.exe,
// ...). origin_id is the kGameRootNativeId pseudo-id so "Open Mod Info" and
// "Hide" stay disabled for it.
static DataTabRow native_root_row(const std::filesystem::path& root,
                                  const std::filesystem::path& file) {
    std::error_code ec;
    DataTabRow row;
    row.path = QString::fromStdString(
        std::filesystem::relative(file, root, ec).generic_string());
    row.vfs_path = row.path;  // game-native files live at the game root already
    row.origin_id = QString::fromStdString(kGameRootNativeId);
    row.source = QCoreApplication::translate("DataTab", "Base Game");
    row.providers = 1;
    row.real_path = QString::fromStdString(file.string());
    row.all_sources = {row.source};
    row.provider_ids = {row.origin_id};
    row.provider_paths = {row.real_path};
    auto sz = std::filesystem::file_size(file, ec);
    if (!ec) row.size = static_cast<qint64>(sz);
    return row;
}

// Recursively collect the game's native root files (dirs relative to root).
static void collect_native_root_rows(const std::filesystem::path& dir,
                                     const std::filesystem::path& root,
                                     const std::string& mods_subpath,
                                     std::vector<DataTabRow>& out) {
    std::error_code ec;
    auto it = std::filesystem::directory_iterator(
        dir, std::filesystem::directory_options::skip_permission_denied, ec);
    for (const auto& entry : it) {
        if (entry.is_directory()) {
            if (is_reserved_root_dir(entry.path().filename().string(), mods_subpath))
                continue;
            collect_native_root_rows(entry.path(), root, mods_subpath, out);
        } else if (entry.is_regular_file()) {
            out.push_back(native_root_row(root, entry.path()));
        }
    }
}

// Every proper path prefix of a row is a directory the tree will show; the
// dirs-first sort keys on this set.
static QSet<QString> dir_set(const std::vector<DataTabRow>& rows) {
    QSet<QString> dirs;
    for (const auto& r : rows) {
        const auto parts = r.path.split('/');
        QString pref;
        for (int i = 0; i + 1 < parts.size(); ++i) {
            if (!pref.isEmpty()) pref += '/';
            pref += parts[i];
            dirs.insert(pref);
        }
    }
    return dirs;
}

// Dirs-first path comparator: at the first diverging component, a directory
// sorts before a file, then alphabetically (the order sort_dirs_first used to
// impose after a path-ordered insert). A proper-prefix path (a directory
// ancestor) sorts before its children. This makes the row insert order equal
// the final tree order, so the main thread's fill needs no post-sort.
static bool dirs_first_less(const QString& ap, const QString& bp, const QSet<QString>& dirs) {
    const auto pa = ap.split('/');
    const auto pb = bp.split('/');
    const int n = qMin(pa.size(), pb.size());
    QString pa_pref, pb_pref;
    for (int i = 0; i < n; ++i) {
        if (!pa_pref.isEmpty()) pa_pref += '/';
        pa_pref += pa[i];
        if (!pb_pref.isEmpty()) pb_pref += '/';
        pb_pref += pb[i];
        if (pa[i] != pb[i]) {
            const bool a_dir = dirs.contains(pa_pref);
            const bool b_dir = dirs.contains(pb_pref);
            if (a_dir != b_dir) return a_dir;
            return pa[i] < pb[i];
        }
    }
    return pa.size() < pb.size();
}

std::vector<DataTabRow> build_data_tab_rows(const DataTabBuildRequest& request) {
    // Snapshot the registry into an index vector so the heavy per-row work
    // (classify + stat + provider enumeration) can fan out across threads via
    // parallel::for_each. The registry is an unordered_map, so the snapshot is
    // just a flat list of pointers - the original keys/owners are read-only
    // for the duration of this pass.
    struct EntryRef {
        const std::string* key;
        const std::vector<std::pair<std::string, int>>* owners;
    };
    std::vector<EntryRef> entries;
    entries.reserve(request.registry.size());
    for (const auto& kv : request.registry) {
        if (kv.second.empty()) continue;
        entries.push_back({&kv.first, &kv.second});
    }

    // Pre-sized slots so each worker thread writes into a unique index
    // without contention. Rows that fail the view filter stay as default-
    // constructed (empty path) sentinels and are dropped in the compact step.
    std::vector<DataTabRow> rows(entries.size());

    // Per-row work: classify + build. Read-only access to the registry, the
    // display-names map, and root_override_mods - the only write is to the
    // worker's own `rows[i]` slot, so no external synchronization is needed.
    const auto& display_names = request.display_names;
    const auto& root_override_mods = request.root_override_mods;
    const auto& mods_dir = request.mods_dir;
    const auto& game_mods_dir = request.game_mods_dir;
    const bool conflict_reversed = request.conflict_reversed;
    const bool root_view = request.root_view;
    const auto& deploy_prefix = request.deploy_prefix;
    const bool deploy_include_mod_id = request.deploy_include_mod_id;
    engine::parallel::for_each(entries.size(), [&](size_t i) {
        const auto& key = *entries[i].key;
        const auto& owners = *entries[i].owners;
        const auto cls = engine::classify_registry_path(
            key, owners, root_override_mods, deploy_prefix);
        if (root_view && cls.space != engine::DeploySpace::Root) return;
        if (!root_view && cls.space != engine::DeploySpace::Data) return;
        rows[i] = build_data_row(key, cls.display_path, owners, display_names,
                                 conflict_reversed, mods_dir, game_mods_dir,
                                 cls.space, deploy_prefix,
                                 deploy_include_mod_id, root_override_mods);
    });

    // Compact: drop the empty-path sentinels left behind by filtered rows.
    // Cheap compared to the per-row stat pass above.
    std::vector<DataTabRow> kept;
    kept.reserve(rows.size());
    for (auto& r : rows) {
        if (!r.path.isEmpty()) kept.push_back(std::move(r));
    }
    rows = std::move(kept);

    // Game-native root files only exist at the game root (skse64_loader.exe,
    // ControlMap_Custom.txt, ...); the data dir shows mod content alone.
    // This filesystem walk stays sequential - it has to push in tree order,
    // and the recursive directory_iterator is hard to parallelize safely.
    if (request.root_view && !request.game_root_dir.empty()) {
        collect_native_root_rows(request.game_root_dir, request.game_root_dir,
                                 request.mods_subpath, rows);
    }

    // Dirs-first sort: rows sharing a display path order the visible copy
    // first so it claims the row and the dimmed duplicate is skipped by the
    // tree fill.
    const auto dirs = dir_set(rows);
    std::sort(rows.begin(), rows.end(),
              [&dirs](const DataTabRow& a, const DataTabRow& b) {
                  if (a.path != b.path) return dirs_first_less(a.path, b.path, dirs);
                  return !a.hidden && b.hidden;
              });
    return rows;
}

// --- Worker thread plumbing (ConflictScanThread shape) ---

DataTabBuildWorker::DataTabBuildWorker(QObject* parent) : QObject(parent) {}

void DataTabBuildWorker::run(DataTabBuildRequest request, quint64 generation) {
    DataTabBuildResult result;
    result.rows = build_data_tab_rows(request);
    emit finished(std::move(result), generation);
}

DataTabBuildThread::DataTabBuildThread(QObject* parent) : QObject(parent) {
    qRegisterMetaType<ui::DataTabBuildResult>();
    thread_ = new QThread(this);
    thread_->setObjectName(QStringLiteral("gmm-data-tab-build"));
    worker_ = new DataTabBuildWorker(nullptr);
    worker_->moveToThread(thread_);
    connect(thread_, &QThread::finished, worker_, &QObject::deleteLater);
    thread_->start();
}

DataTabBuildThread::~DataTabBuildThread() {
    thread_->quit();
    thread_->wait();
}

void DataTabBuildThread::start(DataTabBuildRequest request, quint64 generation) {
    DataTabBuildWorker* worker = worker_;
    QMetaObject::invokeMethod(
        worker,
        [worker, req = std::move(request), gen = generation]() mutable {
            worker->run(std::move(req), gen);
        },
        Qt::QueuedConnection);
}

}  // namespace ui
