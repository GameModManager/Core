#include "ui/panels/data_tab.h"
#include "ui/panels/panel_utils.h"
#include "ui/main_window/data_tab_build_worker.h"
#include "ui/widgets/column_toggle_header.h"
#include "ui/widgets/mod_list_model.h"

#include "engine/deploy/deploy_utils.h"
#include "engine/deploy/root_override.h"
#include "engine/core/log/logger.h"

#include <QAction>
#include <QApplication>
#include <QColor>
#include <QDesktopServices>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHash>
#include <QHeaderView>
#include <QIcon>
#include <QMenu>
#include <QShowEvent>
#include <QSize>
#include <QTextStream>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ui {

// --- MIME icon + tree helpers ---
namespace {

QIcon icon_for_file(const QString& file_path) {
    static QFileIconProvider prov;
    auto px = prov.icon(QFileInfo(file_path)).pixmap(16, 16);
    return QIcon(px);
}

QIcon folder_icon() {
    static QIcon folder;
    if (folder.isNull()) {
        folder = engine::IconManager::instance().resolve_icon("folder", QStyle::SP_DirIcon);
    }
    return folder;
}

// Find or create a child tree item by name under parent.
QTreeWidgetItem* ensure_child(QTreeWidgetItem* parent, const QString& name, bool is_dir) {
    for (int i = 0; i < parent->childCount(); ++i) {
        if (parent->child(i)->text(0) == name)
            return parent->child(i);
    }
    auto* item = new QTreeWidgetItem(parent);
    item->setText(0, name);
    if (is_dir)
        item->setIcon(0, folder_icon());
    return item;
}

// Roles on Data tab file items (column 0) - set by show_data, consumed by the
// context menu. Local to this TU; the signals carry the resolved values out.
enum DataTabItemRole {
    DataRealPathRole = Qt::UserRole,   // QString - on-disk path of the winner
    DataVfsPathRole,                   // QString - merged-view path, game-dir
                                       //           relative (deploy-relative)
    DataOriginModRole,                 // QString - winner mod id
    DataHiddenRole,                    // bool - file carries a hidden suffix
    DataProviderPathsRole,             // QStringList - on-disk copy per provider
    DataProviderIdsRole,               // QStringList - provider mod ids
    DataNavRole,                       // int - nav target (kNavTargetData/Root);
                                      //      unset on ordinary rows
};

// Nav-row targets carried by DataNavRole (see DataTab::View).
constexpr int kNavTargetData = 0;
constexpr int kNavTargetRoot = 1;

// Recursively sort a tree so directories come first, then alphabetical.
// Navigation rows (DataNavRole set) are pinned to the front at every level so
// the ".." / data-dir switcher stays reachable regardless of sort order.
void sort_dirs_first(QTreeWidgetItem* parent) {
    std::vector<QTreeWidgetItem*> children;
    children.reserve(parent->childCount());
    for (int i = 0; i < parent->childCount(); ++i)
        children.push_back(parent->child(i));

    auto first_non_nav = std::stable_partition(
        children.begin(), children.end(),
        [](const QTreeWidgetItem* c) { return !c->data(0, DataNavRole).isNull(); });

    std::sort(first_non_nav, children.end(),
              [](const QTreeWidgetItem* a, const QTreeWidgetItem* b) {
                  bool a_dir = a->childCount() > 0;
                  bool b_dir = b->childCount() > 0;
                  if (a_dir != b_dir) return a_dir;
                  return a->text(0) < b->text(0);
              });

    parent->takeChildren();
    for (auto* c : children) parent->addChild(c);
    for (auto* c : children) sort_dirs_first(c);
}

// Extension-based gate for the Preview action / preview window. Mirrors the
// formats PreviewWindow can render (images + text).
static bool can_preview(const QString& path) {
    const QString ext = QFileInfo(path).suffix().toLower();
    static const QStringList image_exts = {"png", "jpg", "jpeg", "webp", "bmp", "gif"};
    static const QStringList text_exts = {"txt", "ini", "cfg", "log", "json", "xml", "meta", "md"};
    return image_exts.contains(ext) || text_exts.contains(ext);
}

// --- Data tab: per-row build + merge helpers. The per-row compute
// (DataTabRow, build_data_row, the root-view native walk) lives in
// data_tab_build_worker.{h,cpp} and is shared by the background full build and
// the incremental install path (apply_mod), so a single row means the same
// thing in both. The helpers below are the widget-touching half: display-name
// lookup and the indexed tree upsert.

// True when the mod list's display-relevant fields (id, name, root-flag) are
// unchanged. Compared before storing so a show_data with identical inputs can
// skip the population pass entirely.
bool same_mod_list(const QVector<ModEntry>& a, const QVector<ModEntry>& b) {
    if (a.size() != b.size()) return false;
    for (int i = 0; i < a.size(); ++i) {
        if (a[i].id != b[i].id || a[i].name != b[i].name ||
            a[i].root_override != b[i].root_override)
            return false;
    }
    return true;
}

// mod id -> display name lookup (used for the source column and tooltips).
std::unordered_map<std::string, QString> build_display_names(const QVector<ModEntry>& all_mods) {
    std::unordered_map<std::string, QString> names;
    for (const auto& m : all_mods)
        names[m.id.toStdString()] = m.name.isEmpty() ? m.id : m.name;
    return names;
}

// Find or create a child tree item by name, indexed by its full display path.
// Unlike ensure_child (a linear child scan, used by ConflictsTab), lookups are
// O(1) - the Data tab's tree can hold tens of thousands of rows, where the
// linear scan made the full rebuild O(n^2). item_index_ is kept in sync by
// every caller.
QTreeWidgetItem* ensure_indexed_child(QTreeWidgetItem* parent, const QString& name,
                                      const QString& full_path, bool is_dir,
                                      QHash<QString, QTreeWidgetItem*>& index) {
    auto it = index.constFind(full_path);
    if (it != index.constEnd()) return *it;
    auto* item = new QTreeWidgetItem(parent);
    item->setText(0, name);
    if (is_dir) item->setIcon(0, folder_icon());
    index.insert(full_path, item);
    return item;
}

// Insert or update one row's tree item. When the row is already present it is
// overwritten in place (provider counts, winner, sizes) so callers never
// recreate items - this is what keeps the install path incremental.
void upsert_data_row(QTreeWidget* tree, const DataTabRow& row,
                     QHash<QString, QTreeWidgetItem*>& index) {
    const QColor dim = QApplication::palette().color(QPalette::Disabled, QPalette::Text);
    auto parts = row.path.split('/');
    auto* parent = tree->invisibleRootItem();
    QString parent_path;
    for (int i = 0; i < parts.size() - 1; ++i) {
        if (!parent_path.isEmpty()) parent_path += '/';
        parent_path += parts[i];
        parent = ensure_indexed_child(parent, parts[i], parent_path, true, index);
    }

    auto* file_item = ensure_indexed_child(parent, parts.last(), row.path, false, index);

    // A hidden copy whose display name is already owned by a visible (or
    // earlier hidden) row stays invisible - the visible copy wins.
    if (row.hidden && !file_item->data(0, DataRealPathRole).toString().isEmpty())
        return;

    file_item->setIcon(0, icon_for_file(parts.last()));
    file_item->setText(1, row.size >= 0 ? format_size(row.size) : QString());
    file_item->setText(2, row.source);
    file_item->setToolTip(2, row.all_sources.join(", "));
    if (row.providers > 1) {
        file_item->setText(3, QString::number(row.providers));
        file_item->setToolTip(3, row.all_sources.join("\n"));
    } else {
        file_item->setText(3, QString());
        file_item->setToolTip(3, QString());
    }

    // Per-file metadata for the context menu (real path, merged-view path,
    // origin mod, hidden state, provider copies for preview navigation).
    file_item->setData(0, DataRealPathRole, row.real_path);
    file_item->setData(0, DataVfsPathRole, row.vfs_path);
    file_item->setData(0, DataOriginModRole, row.origin_id);
    file_item->setData(0, DataHiddenRole, row.hidden);
    file_item->setData(0, DataProviderPathsRole, row.provider_paths);
    file_item->setData(0, DataProviderIdsRole, row.provider_ids);

    if (row.hidden) {
        file_item->setForeground(0, dim);
        file_item->setForeground(2, dim);
        file_item->setToolTip(0, DataTab::tr("Hidden file"));
    } else {
        // Reclaim the slot from a previously inserted hidden copy of the same
        // base name: drop its dim so the visible file renders normally.
        file_item->setData(0, Qt::ForegroundRole, QVariant());
        file_item->setData(2, Qt::ForegroundRole, QVariant());
    }
}

// --- Root view (root-override mods + game-native root files) helpers ---

// Mod ids flagged [General] rootOverride, as displayed in the mod list.
std::unordered_set<std::string> root_override_mod_ids(const QVector<ModEntry>& all_mods) {
    std::unordered_set<std::string> out;
    for (const auto& m : all_mods)
        if (m.root_override) out.insert(m.id.toStdString());
    return out;
}

// Navigation row that switches the merged tree scope (".." up to the game
// root, or the data-dir folder back into the data view). Pinned to the front
// of the tree by insert order (the worker's dirs-first sort keeps it first).
QTreeWidgetItem* make_nav_item(const QString& label, int target) {
    auto* item = new QTreeWidgetItem();
    item->setText(0, label);
    item->setIcon(0, folder_icon());
    item->setData(0, DataNavRole, target);
    return item;
}

}  // anonymous namespace

// --- DataTab ---
DataTab::DataTab(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    tree_ = new QTreeWidget(this);
    tree_->setColumnCount(4);
    tree_->setHeaderLabels({tr("Name"), tr("Size"), tr("Source"), tr("Providers")});
    auto* header = new ColumnToggleHeaderView(Qt::Horizontal, tree_);
    header->set_column_labels({tr("Name"), tr("Size"), tr("Source"), tr("Providers")});
    tree_->setHeader(header);
    header->setSectionsMovable(true);
    header->setStretchLastSection(false);
    header->setSectionResizeMode(0, QHeaderView::Stretch);
    header->setSectionResizeMode(1, QHeaderView::Interactive);
    header->setSectionResizeMode(2, QHeaderView::Interactive);
    header->setSectionResizeMode(3, QHeaderView::Interactive);
    header->resizeSection(1, 90);
    tree_->setAlternatingRowColors(true);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->setIconSize(QSize(16, 16));
    tree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(tree_, 1);

    // MO2 parity: double-click opens/executes a file; right-click shows the
    // file/context menu (see on_custom_context_menu).
    connect(tree_, &QTreeWidget::itemDoubleClicked,
            this, &DataTab::on_item_double_clicked);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree_, &QTreeWidget::customContextMenuRequested,
            this, &DataTab::on_custom_context_menu);

    // Background row building (THREADING.md §0): the per-file stat pass runs on
    // the worker thread; the finished row set arrives via a queued signal and
    // is chunked into the tree on the main thread.
    build_thread_ = new DataTabBuildThread(this);
    connect(build_thread_->worker(), &DataTabBuildWorker::finished,
            this, &DataTab::on_build_finished);
}

DataTab::~DataTab() {
    // Drop the worker thread before the widget dies (its dtor quits and waits
    // for the worker to finish; an in-flight build result is discarded by the
    // receiver being destroyed).
    delete build_thread_;
}

void DataTab::clear_content() {
    // Invalidate any in-flight or in-progress population and wipe the tree.
    ++build_generation_;
    build_pending_ = false;
    pending_rows_.clear();
    pending_pos_ = 0;
    tree_->clear();
    item_index_.clear();
    applied_rows_.clear();
    dirty_ = true;  // next show/refresh repopulates
}

void DataTab::show_data(
    const std::unordered_map<std::string, std::vector<std::pair<std::string, int>>>& registry,
    const QVector<ModEntry>& all_mods,
    bool conflict_reversed,
    const std::filesystem::path& mods_dir,
    const std::filesystem::path& game_mods_dir,
    const std::filesystem::path& game_root_dir,
    const std::string& mods_subpath,
    const std::string& deploy_prefix,
    bool deploy_include_mod_id)
{
    // No-op when nothing display-relevant changed: after a recompute that
    // leaves the registry and mod list untouched, skip the population pass
    // instead of rebuilding the whole tree.
    const bool inputs_changed =
        registry != stored_registry_ ||
        !same_mod_list(stored_mods_, all_mods) ||
        stored_conflict_reversed_ != conflict_reversed ||
        stored_mods_dir_ != mods_dir ||
        stored_game_mods_dir_ != game_mods_dir ||
        stored_game_root_dir_ != game_root_dir ||
        stored_mods_subpath_ != mods_subpath ||
        stored_deploy_prefix_ != deploy_prefix ||
        stored_deploy_include_mod_id_ != deploy_include_mod_id;

    stored_registry_ = registry;
    stored_mods_ = all_mods;
    stored_conflict_reversed_ = conflict_reversed;
    stored_mods_dir_ = mods_dir;
    stored_game_mods_dir_ = game_mods_dir;
    stored_game_root_dir_ = game_root_dir;
    stored_mods_subpath_ = mods_subpath;
    stored_deploy_prefix_ = deploy_prefix;
    stored_deploy_include_mod_id_ = deploy_include_mod_id;

    if (inputs_changed) dirty_ = true;
    if (dirty_ && isVisible()) request_populate();
}

void DataTab::switch_view(View v) {
    if (view_ == v) return;
    view_ = v;
    // A different view shows a different row set - always repopulate, even if
    // the stored inputs are unchanged.
    dirty_ = true;
    if (isVisible()) request_populate();
}

void DataTab::showEvent(QShowEvent*) {
    if (dirty_) request_populate();
}

void DataTab::request_populate() {
    dirty_ = false;
    build_pending_ = true;
    ++build_generation_;
    build_thread_->start(build_request(), build_generation_);
}

DataTabBuildRequest DataTab::build_request() const {
    DataTabBuildRequest req;
    req.registry = stored_registry_;
    req.display_names = build_display_names(stored_mods_);
    req.root_override_mods = root_override_mod_ids(stored_mods_);
    req.conflict_reversed = stored_conflict_reversed_;
    req.root_view = view_ == View::Root;
    req.mods_dir = stored_mods_dir_;
    req.game_mods_dir = stored_game_mods_dir_;
    req.game_root_dir = stored_game_root_dir_;
    req.mods_subpath = stored_mods_subpath_;
    req.deploy_prefix = stored_deploy_prefix_;
    req.deploy_include_mod_id = stored_deploy_include_mod_id_;
    return req;
}

void DataTab::on_build_finished(DataTabBuildResult result, quint64 generation) {
    if (generation != build_generation_) return;  // a newer build superseded it
    build_pending_ = false;
    apply_build_result(std::move(result));
}

void DataTab::apply_build_result(DataTabBuildResult result) {
    // Identical row set for the current view: the tree already shows it, and
    // applying it again would recreate every item and collapse the user's
    // expansion state (rows encode everything display-relevant, so equality
    // is exact).
    if (result.rows == applied_rows_) return;

    tree_->setUpdatesEnabled(false);
    tree_->clear();
    item_index_.clear();

    // Scope navigation row: ".." climbs from the data dir to the game root,
    // the data-dir folder climbs back down. Inserted before the rows; the
    // worker's dirs-first sort keeps it pinned to the front.
    if (!result.rows.empty()) {
        if (view_ == View::Root) {
            const QString label = stored_mods_subpath_.empty()
                ? tr("Data")
                : QString::fromStdString(stored_mods_subpath_);
            tree_->addTopLevelItem(make_nav_item(label, kNavTargetData));
            tree_->topLevelItem(0)->setToolTip(
                0, tr("Open the %1 folder").arg(label));
        } else if (!stored_game_root_dir_.empty()) {
            tree_->addTopLevelItem(make_nav_item(tr(".."), kNavTargetRoot));
            tree_->topLevelItem(0)->setToolTip(0, tr("Up to the game root directory"));
        }
    }

    pending_rows_ = std::move(result.rows);
    pending_pos_ = 0;
    tree_->setUpdatesEnabled(true);

    if (pending_rows_.empty()) {
        applied_rows_.clear();
        engine::Logger::instance().debug("Data tab populated (" +
            std::string(view_ == View::Root ? "root" : "data") + " view): 0 files");
        return;
    }
    QTimer::singleShot(0, this, &DataTab::apply_chunk_step);
}

void DataTab::apply_chunk_step() {
    // Cancelled: apply_mod superseded an in-flight population, so the pending
    // row set was cleared and a fresh build was queued instead.
    if (pending_rows_.empty()) return;

    // Fill the next ~1000 rows in one event-loop turn (with the tree's updates
    // suspended, so one repaint per chunk) and re-queue for the next turn.
    constexpr int kChunk = 1000;
    tree_->setUpdatesEnabled(false);
    int n = 0;
    while (pending_pos_ < pending_rows_.size() && n < kChunk) {
        upsert_data_row(tree_, pending_rows_[pending_pos_], item_index_);
        ++pending_pos_;
        ++n;
    }
    tree_->setUpdatesEnabled(true);

    if (pending_pos_ < pending_rows_.size()) {
        QTimer::singleShot(0, this, &DataTab::apply_chunk_step);
        return;
    }

    applied_rows_ = std::move(pending_rows_);
    engine::Logger::instance().debug("Data tab populated (" +
        std::string(view_ == View::Root ? "root" : "data") + " view): " +
        std::to_string(applied_rows_.size()) + " files");
    // Folders start collapsed (MO2-style: the tree opens with subfolders
    // closed); double-click expands in place.
}

void DataTab::apply_mod(
    const std::unordered_map<std::string, std::vector<std::pair<std::string, int>>>& registry,
    const std::string& mod_id,
    const QVector<ModEntry>& all_mods,
    bool conflict_reversed,
    const std::filesystem::path& mods_dir,
    const std::filesystem::path& game_mods_dir,
    const std::filesystem::path& game_root_dir,
    const std::string& mods_subpath,
    const std::string& deploy_prefix,
    bool deploy_include_mod_id)
{
    if (registry.empty()) return;

    // Keep the stored inputs in sync so a later view switch rebuilds with the
    // freshly installed mod included.
    stored_registry_ = registry;
    stored_mods_ = all_mods;
    stored_conflict_reversed_ = conflict_reversed;
    stored_mods_dir_ = mods_dir;
    stored_game_mods_dir_ = game_mods_dir;
    stored_game_root_dir_ = game_root_dir;
    stored_mods_subpath_ = mods_subpath;
    stored_deploy_prefix_ = deploy_prefix;
    stored_deploy_include_mod_id_ = deploy_include_mod_id;

    // A background population is in flight (building on the worker thread, or
    // its row set still being chunked into the tree). Merging into the
    // half-built tree would interleave stale rows with the new mod's - cancel
    // the in-flight build and let a fresh one supersede it instead.
    if (build_pending_ || !pending_rows_.empty()) {
        ++build_generation_;
        pending_rows_.clear();
        pending_pos_ = 0;
        dirty_ = true;
        if (isVisible()) request_populate();
        return;
    }

    const bool root_view = view_ == View::Root;
    const auto display_names = build_display_names(all_mods);
    const auto root_mods = root_override_mod_ids(all_mods);

    bool any = false;
    for (const auto& [path, owners] : registry) {
        if (owners.empty()) continue;
        bool provides = false;
        for (const auto& [owner, _] : owners) {
            if (owner == mod_id) { provides = true; break; }
        }
        if (!provides) continue;
        const auto cls = engine::classify_registry_path(
            path, owners, root_mods, deploy_prefix);
        if (root_view && cls.space != engine::DeploySpace::Root) continue;
        if (!root_view && cls.space != engine::DeploySpace::Data) continue;
        any = true;
        upsert_data_row(tree_, build_data_row(path, cls.display_path, owners, display_names,
                                              conflict_reversed, mods_dir, game_mods_dir,
                                              cls.space, deploy_prefix,
                                              deploy_include_mod_id, root_mods),
                        item_index_);
    }

    if (any) {
        sort_dirs_first(tree_->invisibleRootItem());
        applied_rows_.clear();  // the in-place merge no longer matches the last build
        engine::Logger::instance().debug(
            "Data tab updated incrementally for installed mod: " + mod_id);
    }
}

void DataTab::on_item_double_clicked(QTreeWidgetItem* item, int column) {
    (void)column;
    if (!item) return;
    // Navigation rows (".." / data-dir folder) switch the merged tree scope.
    if (!item->data(0, DataNavRole).isNull()) {
        switch_view(item->data(0, DataNavRole).toInt() == kNavTargetRoot
                        ? View::Root
                        : View::Data);
        return;
    }
    if (item->data(0, DataRealPathRole).toString().isEmpty()) return;
    open_item(item);
}

void DataTab::open_item(QTreeWidgetItem* item) {
    const QString real_path = item->data(0, DataRealPathRole).toString();
    if (real_path.isEmpty()) return;
    const bool is_exe = real_path.endsWith(".exe", Qt::CaseInsensitive);
    if (is_exe || QFileInfo(real_path).isExecutable())
        emit execute_requested(real_path, is_exe,
                               item->data(0, DataVfsPathRole).toString());
    else
        emit open_requested(real_path);
}

void DataTab::preview_item(QTreeWidgetItem* item) {
    const QString primary = item->data(0, DataRealPathRole).toString();
    if (primary.isEmpty()) return;
    const QStringList paths = item->data(0, DataProviderPathsRole).toStringList();
    const QStringList names = item->data(0, DataProviderIdsRole).toStringList();
    emit preview_requested(primary, paths, names);
}

void DataTab::on_custom_context_menu(const QPoint& pos) {
    auto* item = tree_->itemAt(pos);
    if (!item) return;
    // Navigation rows have no file menu - double-click switches the scope.
    if (!item->data(0, DataNavRole).isNull()) return;

    QMenu menu(this);
    menu.setToolTipsVisible(true);

    // File actions only for leaf items backed by a real file. Directories and
    // empty space get the common menus only (MO2's addDirectoryMenus is a
    // no-op, so this matches).
    if (item->childCount() == 0 && !item->data(0, DataRealPathRole).toString().isEmpty())
        add_file_menus(menu, item);

    add_common_menus(menu);
    menu.exec(tree_->viewport()->mapToGlobal(pos));
}

void DataTab::add_file_menus(QMenu& menu, QTreeWidgetItem* item) {
    const QString real_path = item->data(0, DataRealPathRole).toString();
    const bool hidden = item->data(0, DataHiddenRole).toBool();
    const bool is_exe = real_path.endsWith(".exe", Qt::CaseInsensitive) ||
                        QFileInfo(real_path).isExecutable();

    // Open/Execute, Preview, Add as Executable. The first enabled one of the
    // first three is bolded (MO2). VFS is not a separate menu entry: the plain
    // Execute (executables) and Add-as-Executable both carry the merged
    // DataVfsPathRole so the receiver launches through the overlay (merged
    // view) - exactly what MO2's plain Execute does when a mod is active.
    // Non-executables open by their physical path (Open carries no VFS role).
    auto* open_action = menu.addAction(
        is_exe ? tr("&Execute") : tr("&Open"),
        this, [this, item]() { open_item(item); });
    open_action->setStatusTip(is_exe ? tr("Launches this program (in the merged mod view)")
                                     : tr("Opens this file with its default handler"));

    auto* preview_action = menu.addAction(
        tr("&Preview"), this, [this, item]() { preview_item(item); });
    preview_action->setStatusTip(tr("Previews this file within GameModManager"));
    if (!can_preview(real_path)) {
        preview_action->setEnabled(false);
        preview_action->setStatusTip(
            tr("This file has no preview handler associated with it"));
    }

    menu.addSeparator();

    auto* add_exe_action = menu.addAction(
        tr("&Add as Executable"),
        this, [this, item]() {
            // Carry the merged-view (deploy-relative) path: it is what the
            // launch overlay resolves, never the on-disk mods-folder path. The
            // physical winning copy rides along so the receiver can extract
            // the exe's icon even before any deploy makes the merged path
            // reachable.
            emit add_executable_requested(item->data(0, DataVfsPathRole).toString(),
                                          item->text(0),
                                          item->data(0, DataRealPathRole).toString());
        });
    add_exe_action->setStatusTip(tr("Add this file to the executables list"));
    if (!is_exe) {
        add_exe_action->setEnabled(false);
        add_exe_action->setStatusTip(tr("This file is not executable"));
    }

    auto* reveal_action = menu.addAction(
        tr("Reveal in E&xplorer"), this, [item]() {
            const auto dir = QFileInfo(item->data(0, DataRealPathRole).toString())
                                 .absolutePath();
            if (!dir.isEmpty())
                QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
        });
    reveal_action->setStatusTip(tr("Opens the file in the file manager"));

    const QString origin_mod_id = item->data(0, DataOriginModRole).toString();
    const bool game_native = origin_mod_id == QLatin1String(kGameRootNativeId);
    auto* mod_info_action = menu.addAction(
        tr("Open &Mod Info"), this, [this, origin_mod_id]() {
            emit open_mod_info_requested(origin_mod_id);
        });
    const bool managed = !origin_mod_id.isEmpty() &&
        origin_mod_id != QLatin1String(kOverwriteModId) &&
        origin_mod_id != QLatin1String(kMergedModId) &&
        !game_native;
    mod_info_action->setStatusTip(tr("Opens the Mod Info Window"));
    if (!managed) {
        mod_info_action->setEnabled(false);
        mod_info_action->setStatusTip(tr("This file is not in a managed mod"));
    }

    auto* hide_action = menu.addAction(
        hidden ? tr("&Un-Hide") : tr("&Hide"),
        this, [this, item, hidden]() {
            emit hide_requested(item->data(0, DataRealPathRole).toString(),
                                item->data(0, DataOriginModRole).toString(),
                                !hidden);
        });
    hide_action->setStatusTip(hidden ? tr("Un-hides the file")
                                     : tr("Hides the file"));
    if (game_native) {
        hide_action->setEnabled(false);
        hide_action->setStatusTip(tr("This file belongs to the game"));
    }
}

void DataTab::add_common_menus(QMenu& menu) {
    menu.addSeparator();

    auto* save_action = menu.addAction(
        tr("&Save Tree to Text File..."), this, [this]() { dump_tree_to_file(); });
    save_action->setStatusTip(tr("Writes the list of files to a text file"));

    auto* refresh_action = menu.addAction(
        tr("&Refresh"), this, [this]() { emit refresh_requested(); });
    refresh_action->setStatusTip(tr("Refreshes the list"));

    menu.addAction(tr("Ex&pand All"), this, [this]() { tree_->expandAll(); });
    menu.addAction(tr("&Collapse All"), this, [this]() { tree_->collapseAll(); });
}

void DataTab::dump_tree_to_file() {
    const QString file_path = QFileDialog::getSaveFileName(
        this, tr("Save Tree to Text File"));
    if (file_path.isEmpty()) return;

    QFile file(file_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return;

    QTextStream out(&file);
    std::function<void(QTreeWidgetItem*, int)> write =
        [&](QTreeWidgetItem* parent, int depth) {
            for (int i = 0; i < parent->childCount(); ++i) {
                auto* child = parent->child(i);
                out << QString(depth * 2, ' ') << child->text(0);
                if (child->childCount() == 0) {
                    if (!child->text(1).isEmpty())
                        out << "  (" << child->text(1) << ')';
                    if (!child->text(2).isEmpty())
                        out << "  " << child->text(2);
                }
                out << '\n';
                write(child, depth + 1);
            }
        };
    write(tree_->invisibleRootItem(), 0);
}

}  // namespace ui
