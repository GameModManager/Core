#include "ui/panels/tab_panels.h"
#include "ui/settings/settings.h"

#include "engine/theme/icon_manager.h"

#include <algorithm>

#include <QAction>
#include <QApplication>
#include <QBrush>
#include <QCheckBox>
#include <QColor>
#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFont>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPalette>
#include <QPainter>
#include <QPair>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QSet>
#include <QShowEvent>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextStream>
#include <QTimer>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include "ui/widgets/column_toggle_header.h"
#include "ui/widgets/mod_list_model.h"
#include "ui/widgets/mod_table_view.h"

#include "engine/log/logger.h"
#include "engine/fs_utils.h"
#include "engine/deploy/root_override.h"
#include "engine/source/loverslab_provider.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>

namespace ui {

// Helper: format bytes to human-readable string
static QString format_size(int64_t bytes) {
    if (bytes < 0) return "?";
    const char* units[] = {"B", "KB", "MB", "GB"};
    int unit_idx = 0;
    double size = static_cast<double>(bytes);
    while (size >= 1024.0 && unit_idx < 3) {
        size /= 1024.0;
        unit_idx++;
    }
    if (unit_idx == 0)
        return QString::number(static_cast<int>(size)) + " " + units[unit_idx];
    return QString::number(size, 'f', 1) + " " + units[unit_idx];
}

// Helper to create a standard table
static QTableWidget* make_table(int cols, const QStringList& headers, QWidget* parent) {
    auto* table = new QTableWidget(parent);
    table->setColumnCount(cols);
    table->setHorizontalHeaderLabels(headers);
    table->horizontalHeader()->setStretchLastSection(true);
    table->verticalHeader()->setVisible(false);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    return table;
}

// Helper: state to display string
static QString state_label(DownloadState s) {
    switch (s) {
        case DownloadState::Downloading: return QCoreApplication::translate("DownloadsTab", "Downloading");
        case DownloadState::Paused:      return QCoreApplication::translate("DownloadsTab", "Paused");
        case DownloadState::Complete:    return QCoreApplication::translate("DownloadsTab", "Install");
        case DownloadState::Installed:   return QCoreApplication::translate("DownloadsTab", "Installed");
        case DownloadState::Failed:      return QCoreApplication::translate("DownloadsTab", "Failed");
        case DownloadState::Removed:     return QCoreApplication::translate("DownloadsTab", "Removed");
    }
    return QCoreApplication::translate("DownloadsTab", "Unknown");
}

// Archive extensions the Downloads tab treats as installable: the untracked
// scan surfaces them and external drops (MO2-style) import them into the
// downloads dir.
static const std::vector<std::string>& download_archive_exts() {
    static const std::vector<std::string> exts = {
        ".zip", ".7z", ".tar", ".rar", ".gz", ".bz2", ".xz", ".fomod"};
    return exts;
}

// Case-insensitive check: does `path` carry one of the supported archive
// extensions?
static bool is_archive_path(const std::filesystem::path& path) {
    std::string lower;
    for (char c : path.extension().string())
        lower.push_back(static_cast<char>(std::tolower(
            static_cast<unsigned char>(c))));
    const auto& exts = download_archive_exts();
    return std::find(exts.begin(), exts.end(), lower) != exts.end();
}

// MO2's downloads-tab drag gate: accept only when every URL is a local file
// with a supported archive extension (a single foreign URL rejects the whole
// drag). Non-local (http etc.) URLs are not handled by the tab.
static bool accepts_url_drop(const QMimeData* data) {
    if (!data || !data->hasUrls()) return false;
    const auto urls = data->urls();
    if (urls.isEmpty()) return false;

    for (const auto& url : urls) {
        if (!url.isLocalFile()) return false;
        if (!is_archive_path(url.toLocalFile().toStdString())) return false;
    }
    return true;
}

// A drag carrying exactly one LoversLab download link (a browser URL drag) is
// routed to the same "Add from URL…" flow as the header button. Browsers drag
// URLs as text/uri-list (surfaced through QMimeData::urls()); a bare-text
// fallback covers text-only drags. Returns the URL, or "" when the drop is not
// a single LoversLab link (so the archive-drop gate below still applies).
static std::string loverslab_drop_url(const QMimeData* data) {
    if (!data) return {};
    QString candidate;
    if (data->hasUrls()) {
        const auto urls = data->urls();
        if (urls.size() != 1) return {};
        candidate = urls.first().toString();
    } else if (data->hasText()) {
        candidate = data->text().trimmed();
    } else {
        return {};
    }
    if (candidate.isEmpty()) return {};
    const std::string url = candidate.toStdString();
    return engine::LoversLabProvider::is_loverslab_url(url) ? url : std::string();
}

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
    DataVfsPathRole,                   // QString - merged-view path (deploy-relative)
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

// Pseudo mod id for game-native root files (skse64_loader.exe, ...). Never a
// real mod folder, so it must fail the "managed mod" / hide gates.
constexpr const char* kGameRootNativeId = "__game_root__";

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

// --- Data tab: per-row build + merge helpers. Shared by the full rebuild
// (show_data) and the incremental install path (apply_mod) so a single row
// means the same thing in both.

struct DataTabRow {
    QString path;          // display path (hidden suffix stripped)
    QString real_path;     // on-disk path of the winning copy
    QString origin_id;     // winner mod id
    QString source;
    qint64 size = -1;
    int providers = 0;
    bool hidden = false;   // file carries .gmmhidden / .mohidden
    QStringList all_sources;
    QStringList provider_paths;
    QStringList provider_ids;
};

// mod id -> display name lookup (used for the source column and tooltips).
std::unordered_map<std::string, QString> build_display_names(const QVector<ModEntry>& all_mods) {
    std::unordered_map<std::string, QString> names;
    for (const auto& m : all_mods)
        names[m.id.toStdString()] = m.name.isEmpty() ? m.id : m.name;
    return names;
}

// On-disk path of a provider's copy: instance mods dir first, then the
// game-native mods dir fallback.
std::filesystem::path resolve_mod_file(const std::string& mod_id,
                                       const std::string& rel_path,
                                       const std::filesystem::path& mods_dir,
                                       const std::filesystem::path& game_mods_dir) {
    std::error_code ec;
    auto candidate = mods_dir / mod_id / rel_path;
    if (std::filesystem::exists(candidate, ec)) return candidate;
    if (!game_mods_dir.empty()) {
        ec.clear();
        candidate = game_mods_dir / mod_id / rel_path;
        if (std::filesystem::exists(candidate, ec)) return candidate;
    }
    return {};
}

// Build one row from a registry entry (path -> (mod_id, priority) providers).
DataTabRow build_data_row(const std::string& path,
                          const std::vector<std::pair<std::string, int>>& owners,
                          const std::unordered_map<std::string, QString>& display_names,
                          bool conflict_reversed,
                          const std::filesystem::path& mods_dir,
                          const std::filesystem::path& game_mods_dir) {
    DataTabRow row;
    row.path = QString::fromStdString(path).replace('\\', '/');

    // Winner = the provider that actually takes effect
    auto winner = conflict_reversed
        ? std::min_element(owners.begin(), owners.end(),
                           [](const auto& a, const auto& b) { return a.second < b.second; })
        : std::max_element(owners.begin(), owners.end(),
                           [](const auto& a, const auto& b) { return a.second < b.second; });

    QString winner_id = QString::fromStdString(winner->first);
    row.origin_id = winner_id;
    if (winner_id == QLatin1String(kOverwriteModId)) {
        row.source = DataTab::tr("Overwrite");
    } else if (winner_id == QLatin1String(kMergedModId)) {
        row.source = DataTab::tr("MERGED");
    } else {
        auto it = display_names.find(winner->first);
        row.source = it != display_names.end() ? it->second : winner_id;
    }

    // Hidden files (.gmmhidden here, .mohidden in MO2-imported instances)
    // display under their base name. A visible file with the same base
    // name wins the display slot; otherwise the hidden copy is shown
    // dimmed (the name survives so it can be un-hidden from the tab).
    if (engine::is_hidden_file(std::filesystem::path(path))) {
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
            resolve_mod_file(owner, path, mods_dir, game_mods_dir).string());
    }

    // Size and real path of the winning copy (hidden suffix intact)
    std::error_code ec;
    const auto winner_real = resolve_mod_file(winner->first, path, mods_dir, game_mods_dir);
    if (!winner_real.empty()) {
        row.real_path = QString::fromStdString(winner_real.string());
        auto sz = std::filesystem::file_size(winner_real, ec);
        if (!ec) row.size = static_cast<qint64>(sz);
    }
    return row;
}

// Insert or update one row's tree item. When the row is already present it is
// overwritten in place (provider counts, winner, sizes) so callers never
// recreate items - this is what keeps the install path incremental.
void upsert_data_row(QTreeWidget* tree, const DataTabRow& row) {
    const QColor dim = QApplication::palette().color(QPalette::Disabled, QPalette::Text);
    auto parts = row.path.split('/');
    auto* parent = tree->invisibleRootItem();
    for (int i = 0; i < parts.size() - 1; ++i)
        parent = ensure_child(parent, parts[i], true);

    auto* file_item = ensure_child(parent, parts.last(), false);

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
    file_item->setData(0, DataVfsPathRole, row.path);
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

// One row for a game-native file sitting in the game root (skse64_loader.exe,
// ...). origin_id is the kGameRootNativeId pseudo-id so "Open Mod Info" and
// "Hide" stay disabled for it.
DataTabRow native_root_row(const std::filesystem::path& root,
                           const std::filesystem::path& file) {
    std::error_code ec;
    DataTabRow row;
    row.path = QString::fromStdString(
        std::filesystem::relative(file, root, ec).generic_string());
    row.origin_id = QString::fromStdString(kGameRootNativeId);
    row.source = DataTab::tr("Base Game");
    row.providers = 1;
    row.real_path = QString::fromStdString(file.string());
    row.all_sources = {row.source};
    row.provider_ids = {row.origin_id};
    row.provider_paths = {row.real_path};
    auto sz = std::filesystem::file_size(file, ec);
    if (!ec) row.size = static_cast<qint64>(sz);
    return row;
}

// True if `name` is a game-root folder the Root view must not descend into:
// the game's own data dir (shown by the merged Data view), the instance mods
// dir, overwrite, and the deploy staging dir. Compared case-insensitively
// (Isaac's real folder is "Mods" while its mods_subpath is "mods").
bool is_reserved_root_dir(const std::string& name, const std::string& mods_subpath) {
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

// Recursively collect the game's native root files (dirs relative to root).
void collect_native_root_rows(const std::filesystem::path& dir,
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

// Navigation row that switches the merged tree scope (".." up to the game
// root, or the data-dir folder back into the data view). Pinned to the front
// of the tree by sort_dirs_first.
QTreeWidgetItem* make_nav_item(const QString& label, int target) {
    auto* item = new QTreeWidgetItem();
    item->setText(0, label);
    item->setIcon(0, folder_icon());
    item->setData(0, DataNavRole, target);
    return item;
}

}  // anonymous namespace

// --- PluginsTab ---

// --- Flag-bound tooltip fragments (MO2 PluginList::tooltipData sub-blocks) ---
// Each fragment describes exactly one status emblem; the Flags column shows
// ONLY the fragment of the emblem under the cursor. plugin_tooltip_html()
// composes the same fragments (plus the non-flag header block) into the full
// per-plugin tooltip shown on the name/priority/mod-index cells.

static QString missing_masters_html(const engine::GamePlugin& p) {
    QStringList names;
    for (const auto& s : p.missing_masters)
        names << QString::fromStdString(s);
    return "<br><b>" + PluginsTab::tr("Missing Masters") + "</b>: <b>" +
           names.join(", ") + "</b>";
}

static QString archives_html(const engine::GamePlugin& p) {
    // Fewer than 6 archives are listed inline; more just get the paragraph.
    QString archive_line;
    if (p.archives.size() < 6) {
        QStringList names;
        for (const auto& a : p.archives)
            names << QString::fromStdString(a);
        archive_line = names.join(", ") + "<br>";
    }
    return "<br><b>" + PluginsTab::tr("Loads Archives") + "</b>: " + archive_line +
           PluginsTab::tr("There are Archives connected to this plugin. Their assets "
                          "will be added to your game, overwriting in case of conflicts "
                          "following the plugin order. Loose files will always overwrite "
                          "assets from Archives. (This flag only checks for Archives from "
                          "the same mod as the plugin)");
}

static QString has_ini_html() {
    return "<br><b>" + PluginsTab::tr("Loads INI settings") + "</b>:<br>" +
           PluginsTab::tr("There is an ini file connected to this plugin. Its settings "
                          "will be added to your game settings, overwriting in case of "
                          "conflicts.");
}

static QString esl_html(const engine::GamePlugin& p) {
    const QString type = p.has_master_ext ? "ESM" : "ESP";
    return "<br><br>" +
           PluginsTab::tr("This %1 is flagged as a light plugin (ESL). It will adhere "
                          "to the %1 load order but the records will be loaded in ESL "
                          "space (FE/FF). You can have up to 4096 light plugins in "
                          "addition to other plugin types.")
               .arg(type);
}

static QString esh_html() {
    return "<br><br>" +
           PluginsTab::tr("This ESM is flagged as a medium plugin (ESH). It adheres to "
                          "the ESM load order but loads records in ESH space (FD). You "
                          "can have 256 medium plugins in addition to other plugin types.");
}

static QString both_light_medium_warning_html() {
    return "<br><br>" +
           PluginsTab::tr("WARNING: This plugin is both light and medium flagged. This "
                          "could indicate that the file was saved improperly and may have "
                          "mismatched record references. Use it at your own risk.");
}

static QString dummy_html() {
    return "<br><br>" +
           PluginsTab::tr("This is a dummy plugin. It contains no records and is "
                          "typically used to load a paired archive file.");
}

// GMM-specific lock marker (MO2 has no lock flag): the load-order pin. It
// lives in its own rightmost column, not among the Flags emblems.
static QString locked_column_tooltip() {
    return PluginsTab::tr("This plugin's load order position is locked.");
}

// (token, html) for every emblem the row shows, in MO2 iconData() order. The
// Flags column stores icons and tooltips as two parallel lists built from this,
// so the per-icon hover text can never point at the wrong emblem. The lock is
// intentionally NOT here - it has its own column.
static QVector<QPair<QString, QString>> plugin_flag_fragments(
    const engine::GamePlugin& p) {
    QVector<QPair<QString, QString>> frags;
    if (!p.missing_masters.empty())
        frags << QPair<QString, QString>(QStringLiteral("warning"),
                                         missing_masters_html(p));
    if (p.has_ini)
        frags << QPair<QString, QString>(QStringLiteral("attachment"),
                                         has_ini_html());
    if (!p.archives.empty())
        frags << QPair<QString, QString>(QStringLiteral("archive"),
                                         archives_html(p));
    if (p.is_light_flagged && !p.has_light_ext)
        frags << QPair<QString, QString>(QStringLiteral("awaiting"),
                                         esl_html(p));
    if (p.is_medium_flagged)
        frags << QPair<QString, QString>(QStringLiteral("run"), esh_html());
    if (p.has_no_records)
        frags << QPair<QString, QString>(QStringLiteral("dummy"), dummy_html());
    // The both-light-and-medium warning rides both of those emblems.
    if (p.is_light_flagged && p.is_medium_flagged) {
        const QString warn = both_light_medium_warning_html();
        for (auto& f : frags) {
            if (f.first == QLatin1String("awaiting") ||
                f.first == QLatin1String("run"))
                f.second += warn;
        }
    }
    return frags;
}

// Rich per-plugin tooltip, mirroring MO2's PluginList::tooltipData layout
// (modorganizer/src/pluginlist.cpp:1492): Origin, enforced-notes, header
// metadata, masters, archives/INI paragraphs, ESL/ESH warnings, dummy note and
// diagnostics messages. Shown identically on every non-flags column of the row.
static QString plugin_tooltip_html(const engine::GamePlugin& p) {
    auto truncate = [](const QString& s) {
        QString t = s;
        if (t.length() > 1024) {
            t.truncate(1024);
            t += "...";
        }
        return t;
    };

    QString tip;
    tip += "<b>" + PluginsTab::tr("Origin") + "</b>: " +
           (p.owner_mod.empty() ? PluginsTab::tr("Game Data")
                                : QString::fromStdString(p.owner_mod).toHtmlEscaped());

    if (p.force_loaded)
        tip += "<br><b><i>" +
               PluginsTab::tr("This plugin can't be disabled or moved (enforced by the game).") +
               "</i></b>";

    if (p.form_version != 0)  // Oblivion-style headers have no form version
        tip += "<br><b>" + PluginsTab::tr("Form Version") + "</b>: " +
               QString::number(p.form_version);

    tip += "<br><b>" + PluginsTab::tr("Header Version") + "</b>: " +
           QString::number(p.header_version);

    if (!p.author.empty())
        tip += "<br><b>" + PluginsTab::tr("Author") + "</b>: " +
               truncate(QString::fromStdString(p.author).toHtmlEscaped());

    if (!p.description.empty())
        tip += "<br><b>" + PluginsTab::tr("Description") + "</b>: " +
               truncate(QString::fromStdString(p.description).toHtmlEscaped());

    if (!p.missing_masters.empty())
        tip += missing_masters_html(p);

    // Enabled masters = declared masters minus the absent ones (MO2
    // std::set_difference over masterUnset).
    QStringList enabled;
    for (const auto& m : p.masters) {
        if (std::find(p.missing_masters.begin(), p.missing_masters.end(), m) ==
            p.missing_masters.end())
            enabled << QString::fromStdString(m);
    }
    if (!enabled.isEmpty())
        tip += "<br><b>" + PluginsTab::tr("Enabled Masters") + "</b>: " +
               enabled.join(", ");

    if (!p.archives.empty())
        tip += archives_html(p);

    if (p.has_ini)
        tip += has_ini_html();

    if (p.is_light_flagged && !p.has_light_ext) {
        tip += esl_html(p);
    } else if (p.is_medium_flagged && p.has_master_ext) {
        tip += esh_html();
    }

    if (p.is_light_flagged && p.is_medium_flagged)
        tip += both_light_medium_warning_html();

    if (p.has_no_records)
        tip += dummy_html();

    if (!p.messages.empty()) {
        tip += "<hr><ul style=\"margin-left:15px; -qt-list-indent: 0;\">";
        for (const auto& msg : p.messages)
            tip += "<li>" + QString::fromStdString(msg).toHtmlEscaped() + "</li>";
        tip += "</ul>";
    }

    return tip;
}

// Table subclass that turns a drop between rows into a reorder request
// instead of letting Qt's default InternalMove rearrange items (whose order
// would then drift from the engine's). The engine repopulates on reorder.
// Defined at namespace scope (NOT in an anonymous namespace) so the qualified
// name matches the forward declaration in the header.
class PluginsTab::PluginTable : public QTableWidget {
public:
    using QTableWidget::QTableWidget;

    // Invoked with (from_row, to_row) when a valid reorder drop happens.
    std::function<void(int, int)> on_reorder;

protected:
    void dropEvent(QDropEvent* event) override {
        // The dragged row. Because the base dropEvent is never called (the
        // engine repopulates instead), the model isn't mutated during the
        // drag, so currentRow() still points at the row the user grabbed.
        const int from = currentRow();
        if (from < 0) {
            event->ignore();
            return;
        }
        const QModelIndex idx = indexAt(event->position().toPoint());
        int to;
        switch (dropIndicatorPosition()) {
            case QAbstractItemView::AboveItem:
            case QAbstractItemView::OnItem:
                to = idx.isValid() ? idx.row() : rowCount() - 1;
                break;
            case QAbstractItemView::BelowItem:
                to = idx.isValid() ? idx.row() + 1 : rowCount() - 1;
                break;
            default:  // OnViewport
                to = rowCount() - 1;
                break;
        }
        if (from == to || from + 1 == to) {  // no-op (incl. drop right below itself)
            event->accept();
            return;
        }
        if (on_reorder) on_reorder(from, to);
        event->accept();  // base dropEvent is NOT called: MainWindow repopulates
    }

    // MO2-style deselection: a plain left click on an already-selected row
    // clears the selection (click the selected plugin again -> unselected).
    // Clicks on the enable checkbox (column 0's check indicator) only toggle
    // the check state, keeping the selection, as in MO2. The "was it selected
    // before this click" test must happen at press time (the base press would
    // otherwise select the row before the release can inspect it); the clear
    // happens on release so drag-reorder still starts from a selected row.
    void mousePressEvent(QMouseEvent* event) override {
        press_was_selected_ = false;
        press_on_check_ = false;
        if (event->button() == Qt::LeftButton &&
            event->modifiers() == Qt::NoModifier) {
            const QModelIndex idx = indexAt(event->pos());
            press_was_selected_ =
                idx.isValid() && selectionModel()->isSelected(idx);
            press_on_check_ = idx.isValid() && idx.column() == 0 &&
                              check_indicator_rect(idx).contains(event->pos());
        }
        QTableWidget::mousePressEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton &&
            event->modifiers() == Qt::NoModifier && press_was_selected_ &&
            !press_on_check_) {
            clearSelection();
            event->accept();
            return;
        }
        QTableWidget::mouseReleaseEvent(event);
    }

private:
    QRect check_indicator_rect(const QModelIndex& idx) const {
        QStyleOptionViewItem opt;
        opt.initFrom(this);
        opt.rect = visualRect(idx);
        opt.features |= QStyleOptionViewItem::HasCheckIndicator;
        return style()->subElementRect(QStyle::SE_ItemViewItemCheckIndicator,
                                       &opt, this);
    }

    bool press_was_selected_ = false;
    bool press_on_check_ = false;
};

// MO2-style status emblems for the plugin list Flags column. One QIcon per
// emblem (MO2 renders a horizontal stack via its icon delegate; a QTableWidget
// cell can only carry one DecorationRole icon, so the emblems ride a custom
// role and a FlagsDelegate paints them individually at native size - stacking
// them into one pixmap would make Qt scale the whole stack down to one icon
// slot, the mod-list bug from 2026-08-03). Emblem names mirror MO2's
// iconData() tokens; per-flag hover text comes from plugin_flag_fragments().
static QIcon plugin_flag_icon(const QString& token) {
    auto& icons = engine::IconManager::instance();
    if (token == QLatin1String("warning")) return icons.resolve_icon("plugin-warning");
    if (token == QLatin1String("awaiting")) return icons.resolve_icon("plugin-light");
    if (token == QLatin1String("run")) return icons.resolve_icon("plugin-medium");
    if (token == QLatin1String("locked")) return icons.resolve_icon("plugin-locked");
    if (token == QLatin1String("attachment")) return icons.resolve_icon("plugin-attachment");
    if (token == QLatin1String("archive")) return icons.resolve_icon("plugin-archive");
    if (token == QLatin1String("dummy")) return icons.resolve_icon("plugin-dummy");
    return {};
}

// Column 4 (Locked): QTableWidget draws a bare DecorationRole icon at the
// cell's left edge no matter the item's textAlignment, so center the single
// lock pin by painting the decoration ourselves on top of the default path
// (background / selection / alternating rows still come from the base).
class CenteredIconDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        const QIcon icon = opt.icon;
        opt.icon = QIcon();  // base path draws no decoration
        // drawControl directly: QStyledItemDelegate::paint would re-run
        // initStyleOption and re-add the icon (left-aligned) on top of ours.
        const QWidget* widget = option.widget;
        QStyle* style = widget ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);
        if (icon.isNull()) return;
        const QSize sz = icon.actualSize(option.rect.size());
        const QRect r = QStyle::alignedRect(option.direction, Qt::AlignCenter,
                                            sz, option.rect);
        const QIcon::Mode mode = (option.state & QStyle::State_Selected)
                                     ? QIcon::Selected
                                     : QIcon::Normal;
        icon.paint(painter, r, Qt::AlignCenter, mode, QIcon::Off);
    }
};

QTableWidget* PluginsTab::table() const {
    return table_;
}

PluginsTab::PluginsTab(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    table_ = new PluginTable(0, 5, this);
    table_->setHorizontalHeaderLabels(
        {tr("Plugin Name"), tr("Flags"), tr("Priority"), tr("Mod Index"),
         tr("Locked")});
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->verticalHeader()->setVisible(false);
    // Flags column: one QIcon per emblem under kPluginFlagsRole, painted by the
    // shared FlagsDelegate at native size (a single DecorationRole icon would
    // have squeezed the whole stack into one slot). kPluginFlagTooltipsRole
    // gives the delegate per-emblem hover text. Row heights follow the wrap
    // math when the column is too narrow for every emblem.
    table_->setItemDelegateForColumn(
        1, new ui::FlagsDelegate(PluginsTab::kPluginFlagsRole,
                                 PluginsTab::kPluginFlagTooltipsRole, table_));
    // Locked column centers its single pin (bare icons would sit left-aligned).
    table_->setItemDelegateForColumn(4, new CenteredIconDelegate(table_));
    connect(table_->horizontalHeader(), &QHeaderView::sectionResized, this,
            [this](int logical, int, int) {
                if (logical == 1) relayout_flag_rows();
            });
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setAlternatingRowColors(true);
    // Drag-reorder within the user band only; fixed rows are not draggable.
    table_->setDragDropMode(QAbstractItemView::InternalMove);
    table_->setDefaultDropAction(Qt::MoveAction);
    table_->setDragDropOverwriteMode(false);
    table_->setDropIndicatorShown(true);
    table_->on_reorder = [this](int from, int to) { emit reorder_requested(from, to); };
    connect(table_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (syncing_ || !item || item->column() != 0) return;
        const int row = item->row();
        if (row < 0 || row >= static_cast<int>(names_.size())) return;
        emit toggle_requested(names_[static_cast<size_t>(row)],
                              item->checkState() == Qt::Checked);
    });
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(table_, &QTableWidget::customContextMenuRequested,
            this, &PluginsTab::on_custom_context_menu);
    layout->addWidget(table_);
}

void PluginsTab::set_plugins(const std::vector<engine::GamePlugin>& plugins) {
    syncing_ = true;
    table_->setRowCount(0);
    names_.clear();
    rows_locked_.clear();
    rows_force_loaded_.clear();
    names_.reserve(plugins.size());
    rows_locked_.reserve(plugins.size());
    rows_force_loaded_.reserve(plugins.size());
    table_->setRowCount(static_cast<int>(plugins.size()));

    const QColor missing_color(0xB0, 0x30, 0x30);
    const QColor fixed_color(Qt::gray);

    for (int i = 0; i < static_cast<int>(plugins.size()); ++i) {
        const auto& p = plugins[static_cast<size_t>(i)];
        names_.push_back(p.name);
        rows_locked_.push_back(p.locked);
        rows_force_loaded_.push_back(p.force_loaded);

        // Column 0: name with the enable checkbox folded into the cell
        // (MO2-style). Fixed rows show a checked box that cannot be toggled;
        // missing-master rows can still be checked but the engine rejects the
        // enable with a message.
        auto* name = new QTableWidgetItem(QString::fromStdString(p.name));
        Qt::ItemFlags nf = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        if (p.force_loaded) {
            name->setFlags(nf);  // no checkable, no drag: pinned
            name->setCheckState(Qt::Checked);
            name->setForeground(fixed_color);
        } else {
            nf |= Qt::ItemIsUserCheckable;
            if (!p.locked) nf |= Qt::ItemIsDragEnabled;  // locked = immovable
            name->setFlags(nf);
            name->setCheckState(p.enabled ? Qt::Checked : Qt::Unchecked);
        }
        // MO2-style type indication via the name font (PluginList::fontData
        // parity): bold = master/light extension, italic = light, underline =
        // medium. Applies to fixed rows too (natives are bold masters).
        if (p.has_master_ext || p.is_master_flagged || p.has_light_ext ||
            p.is_light_flagged || p.is_medium_flagged) {
            QFont f = name->font();
            if (p.has_master_ext || p.is_master_flagged || p.has_light_ext)
                f.setBold(true);
            if (p.is_light_flagged || p.has_light_ext)
                f.setItalic(true);
            if (p.is_medium_flagged)
                f.setUnderline(true);
            name->setFont(f);
        }
        if (p.missing_master) {
            QFont f = name->font();
            f.setItalic(true);
            name->setFont(f);
            name->setForeground(missing_color);
        }

        // Emblems (MO2 PluginList::iconData token order) with their per-flag
        // hover text, both built from plugin_flag_fragments() so icons and
        // tooltips stay parallel. The name font carries the plugin type
        // instead. The flags cell shows ONLY the emblem's own info on hover;
        // the name/priority/mod-index cells keep the full rich tooltip.
        const auto flag_frags = plugin_flag_fragments(p);
        QList<QIcon> flag_icons;
        QStringList flag_tips;
        flag_icons.reserve(flag_frags.size());
        flag_tips.reserve(flag_frags.size());
        for (const auto& f : flag_frags) {
            flag_icons << plugin_flag_icon(f.first);
            flag_tips << f.second;
        }

        const QString tooltip = plugin_tooltip_html(p);
        name->setToolTip(tooltip);
        table_->setItem(i, 0, name);

        // Column 1: status emblems.
        auto* flags = new QTableWidgetItem;
        Qt::ItemFlags ff = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        if (!p.force_loaded && !p.locked) ff |= Qt::ItemIsDragEnabled;
        flags->setFlags(ff);
        if (!flag_icons.isEmpty())
            flags->setData(kPluginFlagsRole, QVariant::fromValue(flag_icons));
        if (!flag_tips.isEmpty())
            flags->setData(kPluginFlagTooltipsRole, QVariant::fromValue(flag_tips));
        flags->setToolTip(QString());  // per-emblem hover only, no full-row tooltip
        table_->setItem(i, 1, flags);

        // Column 2: priority.
        auto* prio = new QTableWidgetItem(QString::number(p.priority));
        Qt::ItemFlags pf = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        if (!p.force_loaded && !p.locked) pf |= Qt::ItemIsDragEnabled;
        prio->setFlags(pf);
        prio->setTextAlignment(Qt::AlignCenter);
        if (p.force_loaded) prio->setForeground(fixed_color);
        prio->setToolTip(tooltip);
        table_->setItem(i, 2, prio);

        // Column 3: mod index.
        auto* idx = new QTableWidgetItem(QString::fromStdString(p.mod_index_text));
        Qt::ItemFlags xf = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        if (!p.force_loaded && !p.locked) xf |= Qt::ItemIsDragEnabled;
        idx->setFlags(xf);
        idx->setTextAlignment(Qt::AlignCenter);
        if (p.force_loaded) idx->setForeground(fixed_color);
        idx->setToolTip(tooltip);
        table_->setItem(i, 3, idx);

        // Column 4: load-order lock (GMM-specific, not in MO2's emblem set).
        // Icon-only cell, empty for unlocked rows; CenteredIconDelegate centers
        // the pin that used to ride the Flags column.
        auto* lock = new QTableWidgetItem;
        Qt::ItemFlags lf = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        if (!p.force_loaded && !p.locked) lf |= Qt::ItemIsDragEnabled;
        lock->setFlags(lf);
        if (p.locked) {
            lock->setIcon(plugin_flag_icon(QLatin1String("locked")));
            lock->setToolTip(locked_column_tooltip());
        }
        table_->setItem(i, 4, lock);
    }
    syncing_ = false;
    apply_highlights();  // rows were rebuilt; re-tint selected-mod/master rows
    relayout_flag_rows();  // row heights follow the emblem wrap math
}

void PluginsTab::relayout_flag_rows() {
    const int col_width = table_->columnWidth(1);
    const int default_h = table_->verticalHeader()->defaultSectionSize();
    for (int i = 0; i < table_->rowCount(); ++i) {
        const auto* item = table_->item(i, 1);
        const QList<QIcon> icons =
            item ? item->data(kPluginFlagsRole).value<QList<QIcon>>()
                 : QList<QIcon>();
        const QSize wrapped = ui::flags_wrapped_size(icons, col_width);
        const int h = wrapped.height() > 0 ? std::max(default_h, wrapped.height())
                                           : default_h;
        table_->setRowHeight(i, h);
    }
}

void PluginsTab::sync_enabled(const std::vector<engine::GamePlugin>& plugins) {
    syncing_ = true;
    const int rows = std::min(static_cast<int>(plugins.size()), table_->rowCount());
    for (int i = 0; i < rows; ++i) {
        const auto& p = plugins[static_cast<size_t>(i)];
        QTableWidgetItem* item = table_->item(i, 0);
        if (!item || p.force_loaded) continue;
        item->setCheckState(p.enabled ? Qt::Checked : Qt::Unchecked);
    }
    syncing_ = false;
}

void PluginsTab::add_context_menu_actions(QMenu& menu, int row) {
    if (row < 0 || row >= static_cast<int>(names_.size())) return;
    const size_t r = static_cast<size_t>(row);

    // MO2's lock actions (PluginListContextMenu): "Lock load order" for
    // unlocked plugins, "Unlock load order" for locked ones. Core rows cannot
    // be locked (the engine refuses).
    const bool locked = r < rows_locked_.size() && rows_locked_[r];
    const bool core = r < rows_force_loaded_.size() && rows_force_loaded_[r];
    if (!locked && !core) {
        menu.addAction(tr("Lock load order"), this, [this, r]() {
            emit lock_requested(names_[r], true);
        });
    } else if (locked) {
        menu.addAction(tr("Unlock load order"), this, [this, r]() {
            emit lock_requested(names_[r], false);
        });
    }
}

void PluginsTab::on_custom_context_menu(const QPoint& pos) {
    const int row = table_->rowAt(pos.y());
    QMenu menu(this);
    add_context_menu_actions(menu, row);
    if (menu.actions().isEmpty()) return;
    menu.exec(table_->viewport()->mapToGlobal(pos));
}

void PluginsTab::apply_highlights() {
    const QColor contained_color = Settings::instance().plugin_list_contained();
    const QColor master_color = Settings::instance().plugin_list_master();
    for (int i = 0; i < table_->rowCount(); ++i) {
        if (static_cast<size_t>(i) >= names_.size()) continue;
        const QString name = QString::fromStdString(names_[static_cast<size_t>(i)]);
        const bool is_contained = contained_names_.contains(name);
        const bool is_master = master_names_.contains(name);
        // Contained wins over master, matching MO2's PluginList check order.
        // Rows in neither set are explicitly cleared (NoBrush) so a changed
        // selection never leaves stale tints from a previously selected mod.
        const QBrush brush = is_contained ? QBrush(contained_color)
                           : is_master    ? QBrush(master_color)
                                          : QBrush();
        for (int c = 0; c < table_->columnCount(); ++c) {
            if (auto* item = table_->item(i, c)) item->setBackground(brush);
        }
    }
}

void PluginsTab::set_contained_plugins(const QVector<QString>& contained) {
    contained_names_ = QSet<QString>(contained.begin(), contained.end());
    apply_highlights();
}

void PluginsTab::set_master_plugins(const QVector<QString>& masters) {
    master_names_ = QSet<QString>(masters.begin(), masters.end());
    apply_highlights();
}

QStringList PluginsTab::selected_plugin_names() const {
    QStringList names;
    if (!table_ || !table_->selectionModel()) return names;
    const auto rows = table_->selectionModel()->selectedRows();
    for (const auto& idx : rows) {
        if (auto* item = table_->item(idx.row(), 0)) names << item->text();
    }
    return names;
}

// --- ArchivesTab ---
ArchivesTab::ArchivesTab(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    tree_ = new QTreeWidget(this);
    tree_->setColumnCount(1);
    tree_->setHeaderHidden(true);
    tree_->setRootIsDecorated(false);
    tree_->setAlternatingRowColors(true);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->setIconSize(QSize(16, 16));
    tree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(tree_, 1);
}

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
}

void DataTab::clear_content() {
    tree_->clear();
}

void DataTab::show_data(
    const std::unordered_map<std::string, std::vector<std::pair<std::string, int>>>& registry,
    const QVector<ModEntry>& all_mods,
    bool conflict_reversed,
    const std::filesystem::path& mods_dir,
    const std::filesystem::path& game_mods_dir,
    const std::filesystem::path& game_root_dir,
    const std::string& mods_subpath,
    const std::string& deploy_prefix)
{
    stored_registry_ = registry;
    stored_mods_ = all_mods;
    stored_conflict_reversed_ = conflict_reversed;
    stored_mods_dir_ = mods_dir;
    stored_game_mods_dir_ = game_mods_dir;
    stored_game_root_dir_ = game_root_dir;
    stored_mods_subpath_ = mods_subpath;
    stored_deploy_prefix_ = deploy_prefix;
    rebuild_from_stored();
}

void DataTab::switch_view(View v) {
    if (view_ == v) return;
    view_ = v;
    rebuild_from_stored();
}

void DataTab::rebuild_from_stored() {
    tree_->clear();
    if (stored_registry_.empty()) return;

    const bool root_view = view_ == View::Root;
    const auto display_names = build_display_names(stored_mods_);
    const auto root_mods = root_override_mod_ids(stored_mods_);

    std::vector<DataTabRow> rows;
    rows.reserve(stored_registry_.size());
    for (const auto& [path, owners] : stored_registry_) {
        if (owners.empty()) continue;
        const auto cls = engine::classify_registry_path(
            path, owners, root_mods, stored_deploy_prefix_);
        if (root_view && cls.space != engine::DeploySpace::Root) continue;
        if (!root_view && cls.space != engine::DeploySpace::Data) continue;
        rows.push_back(build_data_row(cls.display_path, owners, display_names,
                                      stored_conflict_reversed_, stored_mods_dir_,
                                      stored_game_mods_dir_));
    }

    // Game-native root files only exist at the game root (skse64_loader.exe,
    // ControlMap_Custom.txt, ...); the data dir shows mod content alone.
    if (root_view && !stored_game_root_dir_.empty()) {
        collect_native_root_rows(stored_game_root_dir_, stored_game_root_dir_,
                                 stored_mods_subpath_, rows);
    }

    // Sorted path order gives naturally grouped tree insertion. Equal display
    // paths (a hidden copy vs a visible file of the same name) order the
    // visible one first so it claims the row and the dimmed duplicate is
    // skipped in upsert_data_row.
    std::sort(rows.begin(), rows.end(),
              [](const DataTabRow& a, const DataTabRow& b) {
                  if (a.path != b.path) return a.path < b.path;
                  return !a.hidden && b.hidden;
              });

    // Scope navigation row: ".." climbs from the data dir to the game root,
    // the data-dir folder climbs back down. Pinned to the front by
    // sort_dirs_first.
    if (root_view) {
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

    for (const auto& row : rows)
        upsert_data_row(tree_, row);

    sort_dirs_first(tree_->invisibleRootItem());

    engine::Logger::instance().debug("Data tab populated (" +
        std::string(root_view ? "root" : "data") + " view): " +
        std::to_string(rows.size()) + " files");
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
    const std::string& deploy_prefix)
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
        upsert_data_row(tree_, build_data_row(cls.display_path, owners, display_names,
                                              conflict_reversed, mods_dir, game_mods_dir));
    }

    if (any) {
        sort_dirs_first(tree_->invisibleRootItem());
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
        emit execute_requested(real_path, is_exe);
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

    // Open/Execute, Preview, Open with VFS / Execute with VFS. The first
    // enabled one of these three is bolded (MO2). VFS launches are disabled:
    // no VFS machinery exists for arbitrary executables yet.
    auto* open_action = menu.addAction(
        is_exe ? tr("&Execute") : tr("&Open"),
        this, [this, item]() { open_item(item); });
    open_action->setStatusTip(is_exe ? tr("Launches this program")
                                     : tr("Opens this file with its default handler"));

    auto* preview_action = menu.addAction(
        tr("&Preview"), this, [this, item]() { preview_item(item); });
    preview_action->setStatusTip(tr("Previews this file within GameModManager"));
    if (!can_preview(real_path)) {
        preview_action->setEnabled(false);
        preview_action->setStatusTip(
            tr("This file has no preview handler associated with it"));
    }

    auto* hooked_action = menu.addAction(
        is_exe ? tr("Execute with &VFS") : tr("Open with &VFS"), this, []() {});
    hooked_action->setEnabled(false);
    const QString vfs_hint = tr("Not implemented due to platform constraints for now");
    hooked_action->setToolTip(vfs_hint);
    hooked_action->setStatusTip(vfs_hint);

    for (int i = 0; i < 3 && i < menu.actions().size(); ++i) {
        if (menu.actions()[i]->isEnabled()) {
            QFont f = menu.actions()[i]->font();
            f.setBold(true);
            menu.actions()[i]->setFont(f);
            break;
        }
    }

    menu.addSeparator();

    auto* add_exe_action = menu.addAction(
        tr("&Add as Executable"),
        this, [this, item]() {
            // Carry the merged-view (deploy-relative) path: it is what the
            // launch overlay resolves, never the on-disk mods-folder path.
            emit add_executable_requested(item->data(0, DataVfsPathRole).toString(),
                                          item->text(0));
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

// --- SavesTab ---
SavesTab::SavesTab(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    table_ = make_table(2, {tr("Name"), tr("File")}, this);
    layout->addWidget(table_);
}

// --- DownloadsTab ---
DownloadsTab::DownloadsTab(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* top = new QHBoxLayout;
    top->setContentsMargins(4, 2, 4, 2);
    hide_installed_ = new QCheckBox(tr("Hide installed"), this);
    top->addWidget(hide_installed_);
    top->addStretch(1);
    auto* add_url_btn = new QPushButton(tr("Add from URL…"), this);
    add_url_btn->setObjectName("addUrlBtn");
    top->addWidget(add_url_btn);
    layout->addLayout(top);

    table_ = make_table(4, {tr("Name"), tr("Source"), tr("Status"), tr("Size")}, this);
    table_->setObjectName("downloadsTable");
    layout->addWidget(table_, 1);
    apply_compact_style();

    connect(table_, &QTableWidget::cellDoubleClicked,
            this, &DownloadsTab::on_cell_double_clicked);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(table_, &QTableWidget::customContextMenuRequested,
            this, &DownloadsTab::on_custom_context_menu);
    connect(hide_installed_, &QCheckBox::toggled, this, [this](bool checked) {
        Settings::instance().set_hide_installed_downloads(checked);
        apply_installed_filter();
    });

    hide_installed_->setChecked(Settings::instance().hide_installed_downloads());

    // "Add from URL…": paste a LoversLab ?do=download link (LoversLab has no
    // API, so downloads ride the user's session cookie set in Settings). The
    // tab only captures the URL - validation/routing happens in MainWindow.
    connect(add_url_btn, &QPushButton::clicked, this, [this]() {
        bool ok = false;
        const QString url = QInputDialog::getText(
            this, tr("Download from URL"),
            tr("Paste a LoversLab download link:\n"
               "Right-click a file's Download button on loverslab.com and copy\n"
               "the link address (the URL ends in ?do=download).\n\n"
               "Requires a session cookie - set it under Settings > Sources > "
               "LoversLab."),
            QLineEdit::Normal, QString(), &ok);
        if (ok && !url.trimmed().isEmpty()) {
            emit loverslab_url_entered(url.trimmed().toStdString());
        }
    });

    // External archive drops (drag from a file manager) land in the downloads
    // dir and surface as "Manual" entries. The QTableWidget itself does not
    // accept drops, so events propagate up to this widget.
    setAcceptDrops(true);

    // Default conflict resolver: MO2-style question dialog
    // (Overwrite / Rename new file / Ignore file).
    conflict_resolver_ = [this](const std::filesystem::path& existing,
                                const std::filesystem::path& dropped) {
        QMessageBox box(QMessageBox::Question,
                        QString::fromStdString(dropped.filename().string()),
                        tr("A file with the same name has already been "
                           "downloaded. What would you like to do?"));
        box.addButton(tr("Overwrite"), QMessageBox::ActionRole);
        box.addButton(tr("Rename new file"), QMessageBox::YesRole);
        box.addButton(tr("Ignore file"), QMessageBox::RejectRole);
        box.exec();
        switch (box.buttonRole(box.clickedButton())) {
            case QMessageBox::RejectRole:
                return DropConflictAction::Ignore;
            case QMessageBox::ActionRole:
                return DropConflictAction::Overwrite;
            default:
            case QMessageBox::YesRole:
                return DropConflictAction::Rename;
        }
    };

    // Watch the downloads dir so external changes (file-manager copies,
    // modifier drops, removals) surface immediately. directoryChanged fires
    // for any add/remove/rename; the single-shot timer coalesces bursts (Qt
    // may emit several events for a single operation).
    dir_watcher_ = new QFileSystemWatcher(this);
    scan_timer_ = new QTimer(this);
    scan_timer_->setSingleShot(true);
    scan_timer_->setInterval(200);
    connect(dir_watcher_, &QFileSystemWatcher::directoryChanged,
            this, &DownloadsTab::on_downloads_dir_changed);
    connect(scan_timer_, &QTimer::timeout,
            this, &DownloadsTab::on_scan_timer_timeout);
}

void DownloadsTab::apply_compact_style() {
    const bool compact = Settings::instance().compact_downloads();
    table_->setProperty("compact", compact);
    // Explicit heights: independent of any stylesheet/theme, so compact always
    // applies at launch. The dynamic property is kept for theme diagnostics.
    const int h = row_height();
    table_->verticalHeader()->setMinimumSectionSize(h);
    table_->verticalHeader()->setDefaultSectionSize(h);
    for (int r = 0; r < table_->rowCount(); ++r) {
        table_->setRowHeight(r, h);
    }
}

int DownloadsTab::row_height() const {
    const bool compact = Settings::instance().compact_downloads();
    const int base = table_->fontMetrics().height();
    return compact ? qMax(24, base + 8) : qMax(40, base + 22);
}

void DownloadsTab::on_downloads_dir_changed() {
    // Re-arm the debounce timer; a pending scan is deferred so burst events
    // (e.g. a large copy) still coalesce into one refresh.
    if (scan_timer_->isActive()) scan_timer_->stop();
    scan_timer_->start();
    engine::Logger::instance().debug(
        "downloads: dir changed, scan scheduled (entries=" +
        std::to_string(downloads_.size()) + ")");
}

void DownloadsTab::on_scan_timer_timeout() {
    engine::Logger::instance().debug("downloads: scan timer fired");
    scan_downloads_dir();
}

void DownloadsTab::set_conflict_resolver(ConflictResolver resolver) {
    conflict_resolver_ = std::move(resolver);
}

void DownloadsTab::add_download(const std::string& id, const std::string& name,
                                 const std::string& source,
                                 const std::filesystem::path& file_path,
                                 const std::string& nexus_domain,
                                 int file_id,
                                 const std::string& parent_mod_id,
                                 const std::string& page_url) {
    auto [it, inserted] = downloads_.emplace(id, DownloadEntry{});
    if (!inserted) return;  // already exists

    auto& entry = it->second;
    // Append at the end of the table. Do NOT keep a monotonic row counter:
    // remove_entry() reindexes survivors downward, so a stale counter would
    // point past the table end after entries are removed (rows then never
    // surface - the reported "dead tab after removing entries" bug).
    entry.row = table_->rowCount();
    entry.file_path = file_path;
    entry.state = DownloadState::Downloading;
    entry.nexus_domain = nexus_domain;
    entry.file_id = file_id;
    entry.parent_mod_id = parent_mod_id;
    entry.page_url = page_url;

    table_->insertRow(entry.row);

    entry.name_item = new QTableWidgetItem(QString::fromStdString(name));
    entry.name_item->setFlags(entry.name_item->flags() & ~Qt::ItemIsEditable);
    table_->setItem(entry.row, 0, entry.name_item);

    entry.source_item = new QTableWidgetItem(QString::fromStdString(source));
    entry.source_item->setFlags(entry.source_item->flags() & ~Qt::ItemIsEditable);
    entry.source_item->setTextAlignment(Qt::AlignCenter);
    const std::string vendor_key = engine::vendor_icon_key(source);
    if (!vendor_key.empty())
        entry.source_item->setIcon(engine::IconManager::instance().resolve_icon(
            QString::fromStdString(vendor_key)));
    table_->setItem(entry.row, 1, entry.source_item);

    entry.size_item = new QTableWidgetItem(QString());
    entry.size_item->setFlags(entry.size_item->flags() & ~Qt::ItemIsEditable);
    table_->setItem(entry.row, 3, entry.size_item);

    auto* bar = new QProgressBar(table_);
    bar->setRange(0, 100);
    bar->setValue(0);
    bar->setTextVisible(true);
    bar->setFormat("Starting...");
    table_->setCellWidget(entry.row, 2, bar);
    entry.progress_bar = bar;

    table_->setRowHeight(entry.row, row_height());
}

DownloadsTab::DownloadEntry& DownloadsTab::entry_for(const std::string& id) {
    static DownloadEntry null_entry{-1};
    auto it = downloads_.find(id);
    if (it != downloads_.end()) return it->second;
    return null_entry;
}

void DownloadsTab::rename_download(const std::string& id,
                                   const std::string& new_name) {
    auto it = downloads_.find(id);
    if (it == downloads_.end()) return;
    auto& entry = it->second;
    if (entry.name_item) {
        entry.name_item->setText(QString::fromStdString(new_name));
        table_->setRowHeight(entry.row, row_height());
    }
}

void DownloadsTab::update_progress(const std::string& id, int64_t downloaded,
                                    int64_t total, double speed) {
    auto& entry = entry_for(id);
    if (entry.row < 0) return;
    if (entry.state != DownloadState::Downloading) return;

    entry.total_size = total;

    // Size column: show only download speed while downloading
    if (speed > 0.0) {
        entry.size_item->setText(format_size(static_cast<int64_t>(speed)) + "/s");
    } else {
        entry.size_item->setText(QString());
    }

    // Update progress bar
    int pct = 0;
    if (total > 0) {
        pct = static_cast<int>((downloaded * 100) / total);
        if (pct > 100) pct = 100;
    }
    entry.progress_bar->setValue(pct);

    if (speed > 0.0) {
        QString speed_str = format_size(static_cast<int64_t>(speed)) + "/s";
        if (total > 0) {
            entry.progress_bar->setFormat("%p% - " + speed_str);
        } else {
            entry.progress_bar->setFormat(format_size(downloaded) + " - " + speed_str);
        }
    } else {
        if (total > 0) {
            entry.progress_bar->setFormat("%p%");
        } else {
            entry.progress_bar->setFormat(format_size(downloaded));
        }
    }
}

void DownloadsTab::replace_bar_with_label(const std::string& id, const QString& text,
                                           const QColor& bg, const QColor& fg) {
    auto& entry = entry_for(id);
    if (entry.row < 0) return;

    // Remove the progress bar widget
    table_->removeCellWidget(entry.row, 2);
    entry.progress_bar = nullptr;

    // Replace with a centered QTableWidgetItem
    auto* item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    item->setTextAlignment(Qt::AlignCenter);
    if (bg.isValid())
        item->setBackground(bg);
    if (fg.isValid())
        item->setForeground(fg);
    table_->setItem(entry.row, 2, item);
}

void DownloadsTab::mark_complete(const std::string& id, bool success) {
    auto& entry = entry_for(id);
    if (entry.row < 0) return;

    const auto prev_state = entry.state;
    entry.state = success ? DownloadState::Complete : DownloadState::Failed;
    if (success) {
        // Done: normal background, green "Install" text.
        replace_bar_with_label(id, state_label(entry.state), QColor(), QColor("#4CAF50"));
    } else {
        replace_bar_with_label(id, state_label(entry.state),
                               QColor("#f44336"), QColor(Qt::white));
    }

    // Show final archive size
    if (entry.total_size > 0) {
        entry.size_item->setText(format_size(entry.total_size));
    }

    // A download finishing is the only way out of the active-download scan
    // guard; once none remain, re-scan so the completed archive (and anything
    // that landed in the dir meanwhile) surfaces.
    if ((prev_state == DownloadState::Downloading ||
         prev_state == DownloadState::Paused) &&
        !has_active_download())
        on_downloads_dir_changed();
}

void DownloadsTab::mark_installed(const std::string& id) {
    auto& entry = entry_for(id);
    if (entry.row < 0) return;

    entry.state = DownloadState::Installed;
    replace_bar_with_label(id, tr("Installed"), QColor(), QColor());

    // Show final archive size
    if (entry.total_size > 0) {
        entry.size_item->setText(format_size(entry.total_size));
    }

    // If "hide installed" is on, the row disappears immediately
    apply_installed_filter();
}

void DownloadsTab::mark_paused(const std::string& id) {
    auto& entry = entry_for(id);
    if (entry.row < 0) return;

    entry.state = DownloadState::Paused;
    replace_bar_with_label(id, tr("Paused"), QColor("#FF9800"), QColor(Qt::white));
}

void DownloadsTab::mark_downloading(const std::string& id) {
    auto& entry = entry_for(id);
    if (entry.row < 0) return;

    entry.state = DownloadState::Downloading;

    // Drop any label/bar currently in the Status column and start fresh.
    if (entry.progress_bar) {
        entry.progress_bar->deleteLater();
        entry.progress_bar = nullptr;
    }
    table_->removeCellWidget(entry.row, 2);
    delete table_->takeItem(entry.row, 2);

    auto* bar = new QProgressBar(table_);
    bar->setRange(0, 100);
    bar->setValue(0);
    bar->setTextVisible(true);
    bar->setFormat("Starting...");
    table_->setCellWidget(entry.row, 2, bar);
    entry.progress_bar = bar;

    table_->setRowHeight(entry.row, row_height());
}

void DownloadsTab::set_file_path(const std::string& id, const std::filesystem::path& path) {
    auto& entry = entry_for(id);
    if (entry.row < 0) return;
    entry.file_path = path;
}

void DownloadsTab::set_downloads_dir(const std::filesystem::path& dir) {
    if (downloads_dir_ != dir) {
        // Re-point the watcher (a switched instance gets a new downloads dir).
        const auto old_str = QString::fromStdString(downloads_dir_.string());
        if (!old_str.isEmpty() && dir_watcher_->directories().contains(old_str))
            dir_watcher_->removePath(old_str);
        downloads_dir_ = dir;
    }
    if (downloads_dir_.empty()) return;

    std::error_code ec;
    std::filesystem::create_directories(downloads_dir_, ec);
    if (ec) {
        engine::Logger::instance().error("downloads: cannot create dir " +
            downloads_dir_.string() + ": " + ec.message());
    }
    const auto dir_str = QString::fromStdString(downloads_dir_.string());
    if (!dir_watcher_->directories().contains(dir_str))
        dir_watcher_->addPath(dir_str);
    engine::Logger::instance().debug(
        "downloads: watching dir " + downloads_dir_.string() +
        " (watched=" +
        std::to_string(dir_watcher_->directories().contains(dir_str)) + ")");

    // Re-surface folder state immediately (new dir, or re-pointed after an
    // instance switch); the watcher covers later external changes.
    scan_downloads_dir();
}

void DownloadsTab::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    // Re-scan so archives dropped into the downloads dir while the app is
    // running appear in the list.
    scan_downloads_dir();
}

bool DownloadsTab::has_active_download() const {
    for (const auto& [id, entry] : downloads_) {
        (void)id;
        if (entry.state == DownloadState::Downloading ||
            entry.state == DownloadState::Paused)
            return true;
    }
    return false;
}

void DownloadsTab::scan_downloads_dir() {
    if (downloads_dir_.empty()) {
        engine::Logger::instance().debug("downloads: scan skipped (dir empty)");
        return;
    }
    // While a download is downloading or paused its partial archive sits in
    // the dir under its final name; don't surface it as a "Complete" entry.
    if (has_active_download()) {
        engine::Logger::instance().debug("downloads: scan skipped (active download)");
        return;
    }

    std::error_code ec;
    std::filesystem::directory_iterator it(downloads_dir_, ec);
    if (ec) {
        engine::Logger::instance().error("downloads: scan failed for " +
            downloads_dir_.string() + ": " + ec.message());
        return;
    }
    int scanned = 0;
    for (const auto& entry : it) {
        if (!entry.is_regular_file(ec)) continue;
        ++scanned;
        add_downloads_dir_file(entry.path());
    }
    engine::Logger::instance().debug("downloads: scanned " +
        std::to_string(scanned) + " files, tracked=" +
        std::to_string(downloads_.size()));

    // Drop rows whose archive file is gone from the dir, so the tab mirrors
    // disk state (the watchdog can't see a file a file manager just deleted).
    // Collect ids first: remove_entry() mutates downloads_, and this loop is
    // iterating it. remove_entry() skips the trash step for the missing file
    // and emits entry_removed so the manifest persists the removal.
    std::vector<std::string> vanished;
    for (const auto& [id, entry] : downloads_) {
        if (entry.file_path.empty()) continue;
        std::error_code fec;
        if (!std::filesystem::exists(entry.file_path, fec))
            vanished.push_back(id);
    }
    for (const auto& id : vanished)
        remove_entry(id);

    apply_installed_filter();
}

bool DownloadsTab::add_downloads_dir_file(const std::filesystem::path& path) {
    if (!is_archive_path(path)) {
        engine::Logger::instance().debug("downloads: skip non-archive " +
            path.filename().string());
        return false;
    }

    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        engine::Logger::instance().debug("downloads: skip non-file " +
            path.string());
        return false;
    }

    const auto id = path.filename().string();
    if (downloads_.count(id)) {
        // Already tracked under this name: an overwrite drop replaced the
        // file on disk, so refresh the row's size instead of adding a
        // duplicate (the "new archive" must surface immediately).
        auto& existing = downloads_.at(id);
        auto fsize = std::filesystem::file_size(path, ec);
        if (!ec) {
            existing.total_size = static_cast<int64_t>(fsize);
            if (existing.size_item)
                existing.size_item->setText(format_size(existing.total_size));
        }
        return true;
    }

    // Already backs a tracked entry under a different key (Nexus downloads
    // use "<mod_id>-<file_id>", not the filename).
    for (const auto& [eid, e] : downloads_) {
        (void)eid;
        if (e.file_path == path) return false;
    }

    add_download(id, path.stem().string(), tr("Manual").toStdString(), path);
    auto& added = downloads_.at(id);
    added.total_size = static_cast<int64_t>(std::filesystem::file_size(path, ec));
    if (ec) added.total_size = 0;
    mark_complete(id, true);
    engine::Logger::instance().debug("downloads: added Manual entry '" + id +
        "' (total=" + std::to_string(downloads_.size()) + ")");
    return true;
}

bool DownloadsTab::import_dropped_file(const std::filesystem::path& source,
                                       bool move) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(source, ec)) {
        engine::Logger::instance().warn("drop: invalid source file " + source.string());
        return false;
    }

    if (downloads_dir_.empty()) return false;
    std::filesystem::create_directories(downloads_dir_, ec);
    if (ec) {
        engine::Logger::instance().error("drop: cannot create downloads dir " +
            downloads_dir_.string() + ": " + ec.message());
        return false;
    }

    auto dest = downloads_dir_ / source.filename();

    // Dropping a file that already sits in the downloads dir: just surface it.
    std::error_code cmp_ec;
    const bool same_file =
        std::filesystem::equivalent(source, dest, cmp_ec) ||
        (!cmp_ec && source == dest);
    if (same_file)
        return add_downloads_dir_file(dest);

    const bool exists = std::filesystem::exists(dest, ec);
    if (exists) {
        if (!conflict_resolver_) return false;
        switch (conflict_resolver_(dest, source)) {
            case DropConflictAction::Ignore:
                return false;
            case DropConflictAction::Rename: {
                // MO2's getDownloadFileName numbering: N_<name>, 1-based.
                int i = 1;
                while (std::filesystem::exists(
                    downloads_dir_ / (std::to_string(i) + "_" +
                                      source.filename().string()), ec)) {
                    ++i;
                    ec.clear();
                }
                dest = downloads_dir_ / (std::to_string(i) + "_" +
                                         source.filename().string());
                break;
            }
            case DropConflictAction::Overwrite:
                break;
        }
    }

    bool ok = false;
    if (move) {
        ok = engine::move_path(source, dest);
    } else {
        std::error_code copy_ec;
        std::filesystem::copy_file(source, dest,
            std::filesystem::copy_options::overwrite_existing, copy_ec);
        if (!copy_ec) ok = true;
        else engine::Logger::instance().error("drop: failed to copy " + source.string() +
            " -> " + dest.string() + ": " + copy_ec.message());
    }
    if (!ok) return false;

    return add_downloads_dir_file(dest);
}

void DownloadsTab::dragEnterEvent(QDragEnterEvent* event) {
    if (accepts_url_drop(event->mimeData()) ||
        !loverslab_drop_url(event->mimeData()).empty())
        event->acceptProposedAction();
}

void DownloadsTab::dragMoveEvent(QDragMoveEvent* event) {
    if (accepts_url_drop(event->mimeData()) ||
        !loverslab_drop_url(event->mimeData()).empty())
        event->acceptProposedAction();
}

void DownloadsTab::dropEvent(QDropEvent* event) {
    // A single LoversLab download link dropped from a browser starts the same
    // download flow as the "Add from URL…" button.
    const std::string ll_url = loverslab_drop_url(event->mimeData());
    if (!ll_url.empty()) {
        emit loverslab_url_entered(ll_url);
        event->accept();
        return;
    }

    if (!accepts_url_drop(event->mimeData())) {
        event->ignore();
        return;
    }
    const bool move = event->proposedAction() == Qt::MoveAction;
    if (move) {
        // Tell the source we take ownership of the file (MO2 does the same
        // so the file manager does not also move it).
        event->setDropAction(Qt::TargetMoveAction);
    }

    // accepts_url_drop already guarantees every URL is a local archive file.
    for (const auto& url : event->mimeData()->urls())
        import_dropped_file(url.toLocalFile().toStdString(), move);
    apply_installed_filter();
    event->accept();
}

void DownloadsTab::apply_installed_filter() {
    if (!hide_installed_ || !table_) return;
    const bool hide = hide_installed_->isChecked();
    for (const auto& [id, entry] : downloads_) {
        table_->setRowHidden(entry.row, hide && entry.state == DownloadState::Installed);
    }
}

void DownloadsTab::reapply_installed_filter() {
    apply_installed_filter();
}

void DownloadsTab::on_cell_double_clicked(int row, int column) {
    (void)column;
    // Find the download id for this row
    for (const auto& [id, entry] : downloads_) {
        if (entry.row == row) {
            if (entry.state == DownloadState::Complete ||
                entry.state == DownloadState::Failed) {
                if (!entry.file_path.empty() &&
                    std::filesystem::exists(entry.file_path)) {
                    auto src = source_info_for(id, entry);
                    emit install_requested(id, entry.file_path, src.source_type,
                        src.source_id, src.file_id,
                        entry.name_item ? entry.name_item->text().toStdString()
                                        : std::string(),
                        src.page_url);
                }
            }
            return;
        }
    }
}

DownloadsTab::SourceInfo DownloadsTab::source_info_for(
        const std::string& id, const DownloadEntry& entry) const {
    SourceInfo info;
    const QString source_text =
        entry.source_item ? entry.source_item->text() : QString();
    if (source_text == QStringLiteral("Nexus Mods")) {
        info.source_type = "nexus";
        info.source_id = entry.parent_mod_id;
        info.file_id = entry.file_id;
    } else if (source_text == QStringLiteral("LoversLab")) {
        // The entry id is the file id (or an ll-<hash> fallback when the URL
        // carried none); page_url is the mod page the download came from.
        info.source_type = "loverslab";
        info.source_id = id;
        info.page_url = entry.page_url;
    }
    return info;
}

void DownloadsTab::on_custom_context_menu(const QPoint& pos) {
    auto* item = table_->itemAt(pos);
    if (!item) return;
    const int row = item->row();

    const std::string* found = nullptr;
    for (const auto& [id, entry] : downloads_) {
        if (entry.row == row) {
            found = &id;
            break;
        }
    }
    if (!found) return;

    QMenu menu(this);
    add_context_menu_actions(menu, *found);
    menu.exec(table_->viewport()->mapToGlobal(pos));
}

void DownloadsTab::add_context_menu_actions(QMenu& menu, const std::string& id) {
    const auto& entry = downloads_.at(id);

    // Resolved through the central IconManager (theme/pack -> system -> fugue),
    // with a standard-icon fallback for the download actions.
    auto icon_for = [](const QString& theme, QStyle::StandardPixmap fallback) -> QIcon {
        return engine::IconManager::instance().resolve_icon(theme, fallback);
    };

    // Install / Reinstall (enabled when the archive exists and no download
    // is running). Reuses the same pipeline as double-click.
    const bool can_install =
        entry.state != DownloadState::Downloading &&
        entry.state != DownloadState::Paused &&
        !entry.file_path.empty() &&
        std::filesystem::exists(entry.file_path);
    auto* install_action = menu.addAction(
        icon_for("download", QStyle::SP_ArrowDown),
        entry.state == DownloadState::Installed ? tr("Reinstall") : tr("Install"),
        this, [this, id, entry]() {
            // Origin metadata: derive the engine source_type from the Source
            // column (literal strings, serialized, so it survives restarts).
            // LoversLab rows get "loverslab" so a later reinstall keeps the
            // provenance. Not compared via tr(): the column is filled with the
            // literal text, not a translated one.
            auto src = source_info_for(id, entry);
            emit install_requested(id, entry.file_path, src.source_type,
                src.source_id, src.file_id,
                entry.name_item ? entry.name_item->text().toStdString()
                                : std::string(),
                src.page_url);
        });
    install_action->setEnabled(can_install);

    if (entry.state == DownloadState::Downloading) {
        menu.addAction(icon_for("media-playback-pause", QStyle::SP_MediaPause),
            tr("Pause"), this, [this, id]() { emit pause_requested(id); });
    } else if (entry.state == DownloadState::Paused) {
        menu.addAction(icon_for("media-playback-start", QStyle::SP_MediaPlay),
            tr("Resume"), this, [this, id]() { emit resume_requested(id); });
    }

    menu.addAction(icon_for("folder", QStyle::SP_DirOpenIcon),
        tr("Show in Folder"), this, [entry, this]() {
            auto dir = downloads_dir_;
            if (!entry.file_path.empty())
                dir = entry.file_path.parent_path();
            if (!dir.empty())
                QDesktopServices::openUrl(QUrl::fromLocalFile(
                    QString::fromStdString(dir.string())));
        });

    // Open on <Source>: LoversLab rows open the stored mod page URL (the
    // download link minus the ?do=download query, persisted in the manifest);
    // Nexus rows build the page from domain + mod id. Local ("Manual") rows
    // have no page and get no action.
    const QString source_text =
        entry.source_item ? entry.source_item->text() : QString();
    QString page_url;
    QString page_label;
    if (!entry.page_url.empty()) {
        page_label = tr("Open on %1").arg(
            source_text.isEmpty() ? tr("LoversLab") : source_text);
        page_url = QString::fromStdString(entry.page_url);
    } else if (!entry.nexus_domain.empty() && !entry.parent_mod_id.empty()) {
        page_label = tr("Open on Nexus");
        page_url = QString::fromStdString(
            "https://www.nexusmods.com/" + entry.nexus_domain +
            "/mods/" + entry.parent_mod_id);
    }
    if (!page_url.isEmpty()) {
        auto* page_action = menu.addAction(
            icon_for("text-html", QStyle::SP_FileDialogInfoView),
            page_label, this, [page_url]() {
                QDesktopServices::openUrl(QUrl(page_url));
            });
        page_action->setData(page_url);  // test handle for the target URL
    }

    menu.addSeparator();
    menu.addAction(icon_for("edit-delete", QStyle::SP_TrashIcon),
        tr("Remove"), this, [this, id]() { remove_entry(id); });
}

void DownloadsTab::remove_entry(const std::string& id) {
    auto it = downloads_.find(id);
    if (it == downloads_.end()) return;
    auto& entry = it->second;

    // Trash the archive file (if any) before dropping the entry.
    if (!entry.file_path.empty() && std::filesystem::exists(entry.file_path)) {
        if (!engine::remove_path(entry.file_path)) {
            QMessageBox::warning(this, tr("Remove"),
                tr("Failed to move \"%1\" to the trash bin.").arg(
                    QString::fromStdString(entry.file_path.filename().string())));
            return;
        }
    }

    const int removed_row = entry.row;
    table_->removeRow(removed_row);
    downloads_.erase(it);

    // Rows below the removed one shift up - reindex the survivors.
    for (auto& [eid, e] : downloads_) {
        if (e.row > removed_row) --e.row;
    }

    emit entry_removed(id);
}

// --- Manifest persistence ---

std::string DownloadsTab::serialize() const {
    QJsonArray arr;
    for (const auto& [id, entry] : downloads_) {
        QJsonObject obj;
        obj["id"] = QString::fromStdString(id);
        obj["name"] = entry.name_item ? entry.name_item->text() : QString();
        obj["source"] = entry.source_item ? entry.source_item->text() : QString();
        obj["size"] = entry.size_item ? entry.size_item->text() : QString();
        obj["file_path"] = QString::fromStdString(entry.file_path.string());
        obj["state"] = static_cast<int>(entry.state);
        obj["total_size"] = static_cast<qint64>(entry.total_size);
        // Origin metadata - kept for update checks and the future
        // parent-mod + submods grouping (optional files).
        obj["parent_mod_id"] = QString::fromStdString(entry.parent_mod_id);
        obj["file_id"] = entry.file_id;
        obj["domain"] = QString::fromStdString(entry.nexus_domain);
        obj["category"] = QString::fromStdString(entry.category);
        obj["page_url"] = QString::fromStdString(entry.page_url);
        arr.append(obj);
    }
    QJsonDocument doc(arr);
    return doc.toJson(QJsonDocument::Compact).toStdString();
}

void DownloadsTab::deserialize(const std::string& json,
                                const std::filesystem::path& downloads_dir) {
    auto doc = QJsonDocument::fromJson(QString::fromStdString(json).toUtf8());
    if (!doc.isArray()) return;

    for (const auto& val : doc.array()) {
        auto obj = val.toObject();
        auto id = obj["id"].toString().toStdString();
        auto name = obj["name"].toString().toStdString();
        auto source = obj["source"].toString().toStdString();
        auto size_text = obj["size"].toString();
        auto file_path = std::filesystem::path(obj["file_path"].toString().toStdString());
        auto state = static_cast<DownloadState>(obj["state"].toInt());
        auto total_size = static_cast<int64_t>(obj["total_size"].toDouble());

        // Skip if already loaded
        if (downloads_.count(id)) continue;

        // Verify the archive file still exists
        if (!file_path.empty() && !std::filesystem::exists(file_path)) {
            // Try relative to downloads_dir
            auto abs_path = downloads_dir / file_path.filename();
            if (!std::filesystem::exists(abs_path)) continue;
            file_path = abs_path;
        }

        auto [it, inserted] = downloads_.emplace(id, DownloadEntry{});
        if (!inserted) continue;

        auto& entry = it->second;
        entry.row = table_->rowCount();
        entry.file_path = file_path;
        entry.state = state;
        entry.total_size = total_size;
        entry.parent_mod_id = obj["parent_mod_id"].toString().toStdString();
        entry.file_id = obj["file_id"].toInt();
        entry.nexus_domain = obj["domain"].toString().toStdString();
        entry.category = obj["category"].toString().toStdString();
        entry.page_url = obj["page_url"].toString().toStdString();

        table_->insertRow(entry.row);

        entry.name_item = new QTableWidgetItem(QString::fromStdString(name));
        entry.name_item->setFlags(entry.name_item->flags() & ~Qt::ItemIsEditable);
        table_->setItem(entry.row, 0, entry.name_item);

        entry.source_item = new QTableWidgetItem(QString::fromStdString(source));
        entry.source_item->setFlags(entry.source_item->flags() & ~Qt::ItemIsEditable);
        entry.source_item->setTextAlignment(Qt::AlignCenter);
        const std::string vendor_key = engine::vendor_icon_key(source);
        if (!vendor_key.empty())
            entry.source_item->setIcon(engine::IconManager::instance().resolve_icon(
                QString::fromStdString(vendor_key)));
        table_->setItem(entry.row, 1, entry.source_item);

        // For non-downloading states, show the file size; during download the
        // size cell is empty until update_progress sets the speed text.
        if (state == DownloadState::Downloading) {
            entry.size_item = new QTableWidgetItem(QString());
            entry.size_item->setFlags(entry.size_item->flags() & ~Qt::ItemIsEditable);
            table_->setItem(entry.row, 3, entry.size_item);

            auto* bar = new QProgressBar(table_);
            bar->setRange(0, 100);
            bar->setValue(0);
            bar->setTextVisible(true);
            bar->setFormat("Starting...");
            table_->setCellWidget(entry.row, 2, bar);
            entry.progress_bar = bar;
        } else {
            // If total_size wasn't persisted (old manifest), stat the file
            int64_t resolved_size = total_size;
            if (resolved_size == 0 && !file_path.empty()) {
                std::error_code ec;
                auto fsize = std::filesystem::file_size(file_path, ec);
                if (!ec) resolved_size = static_cast<int64_t>(fsize);
            }
            entry.total_size = resolved_size;

            QString display_size = (resolved_size > 0) ? format_size(resolved_size) : QString();
            entry.size_item = new QTableWidgetItem(display_size);
            entry.size_item->setFlags(entry.size_item->flags() & ~Qt::ItemIsEditable);
            table_->setItem(entry.row, 3, entry.size_item);

            QColor bg, fg;
            if (state == DownloadState::Complete) {
                fg = QColor("#4CAF50");
            } else if (state == DownloadState::Failed) {
                bg = QColor("#f44336");
                fg = QColor(Qt::white);
            } else if (state == DownloadState::Removed) {
                fg = QColor("#B8860B");
            }
            auto* item = new QTableWidgetItem(state_label(state));
            item->setFlags(item->flags() & ~Qt::ItemIsEditable);
            item->setTextAlignment(Qt::AlignCenter);
            if (bg.isValid())
                item->setBackground(bg);
            if (fg.isValid())
                item->setForeground(fg);
            table_->setItem(entry.row, 2, item);
        }

        table_->setRowHeight(entry.row, row_height());
    }

    apply_installed_filter();
}

// --- ConflictsTab ---
ConflictsTab::ConflictsTab(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    tree_ = new QTreeWidget(this);
    tree_->setHeaderLabel("Conflicts");
    tree_->setRootIsDecorated(true);
    tree_->setAlternatingRowColors(true);
    tree_->header()->setStretchLastSection(true);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->setAnimated(true);
    tree_->setIconSize(QSize(16, 16));
    layout->addWidget(tree_, 1);

    connect(tree_, &QTreeWidget::itemDoubleClicked,
            this, &ConflictsTab::on_item_double_clicked);

    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree_, &QTreeWidget::customContextMenuRequested,
            this, &ConflictsTab::on_custom_context_menu);
}

void ConflictsTab::clear_content() {
    tree_->clear();
}

void ConflictsTab::show_conflicts(
    const QString& selected_mod_id,
    const QVector<ModEntry>& all_mods,
    const std::unordered_map<std::string, std::vector<std::pair<std::string, int>>>& file_registry,
    const QMap<QString, ConflictPairs>& pairs,
    bool conflict_reversed)
{
    tree_->clear();

    auto it = pairs.find(selected_mod_id);
    if (it == pairs.end()) return;

    const auto& cp = it.value();
    if (cp.wins_against.isEmpty() && cp.loses_to.isEmpty()) return;

    // Build a set of enabled mod IDs - disabled mods have no influence
    QSet<QString> enabled_ids;
    for (const auto& m : all_mods)
        if (m.enabled || m.is_overwrite || m.is_merged)
            enabled_ids.insert(m.id);

    QSet<QString> conflict_mods;
    for (const auto& w : cp.wins_against)
        if (enabled_ids.contains(w)) conflict_mods.insert(w);
    for (const auto& l : cp.loses_to)
        if (enabled_ids.contains(l)) conflict_mods.insert(l);

    QMap<QString, QStringList> mod_files;
    for (const auto& [rel_path, owners] : file_registry) {
        if (owners.size() <= 1) continue;

        bool selected_owns = false;
        for (const auto& [mod, _] : owners) {
            if (mod == selected_mod_id.toStdString()) {
                selected_owns = true;
                break;
            }
        }
        if (!selected_owns) continue;

        for (const auto& [mod, _] : owners) {
            if (mod == selected_mod_id.toStdString()) continue;
            QString qmod = QString::fromStdString(mod);
            if (conflict_mods.contains(qmod))
                mod_files[qmod].append(QString::fromStdString(rel_path));
        }
    }

    QMap<QString, int> priorities;
    for (const auto& m : all_mods)
        priorities[m.id] = m.priority;

    auto sort_by_prio = [&](const QStringList& list, QStringList& out) {
        out = list;
        std::sort(out.begin(), out.end(),
            [&](const QString& a, const QString& b) {
                return priorities.value(a, 0) < priorities.value(b, 0);
            });
    };
    QStringList sorted_wins, sorted_losses;
    sort_by_prio(cp.wins_against, sorted_wins);
    sort_by_prio(cp.loses_to, sorted_losses);
    auto sorted_mods = sorted_wins + sorted_losses;

    QColor win_color{100, 180, 100};
    QColor lose_color{220, 100, 100};

    for (const auto& mod_id : sorted_mods) {
        bool is_win = cp.wins_against.contains(mod_id);

        QString display_name = mod_id;
        for (const auto& m : all_mods) {
            if (m.id == mod_id) {
                display_name = m.name;
                break;
            }
        }

        auto* mod_item = new QTreeWidgetItem(tree_);
        mod_item->setText(0, display_name);
        mod_item->setToolTip(0, mod_id);
        mod_item->setIcon(0, folder_icon());
        mod_item->setForeground(0, is_win ? win_color : lose_color);
        QFont f = mod_item->font(0);
        f.setBold(true);
        mod_item->setFont(0, f);

        auto files = mod_files.value(mod_id);
        std::sort(files.begin(), files.end());
        for (const auto& fp : files) {
            auto parts = fp.split('/');
            auto* parent = mod_item;
            for (int i = 0; i < parts.size() - 1; ++i)
                parent = ensure_child(parent, parts[i], true);
            // Last segment is the filename
            auto* file_item = ensure_child(parent, parts.last(), false);
            file_item->setIcon(0, icon_for_file(parts.last()));
            file_item->setData(0, Qt::UserRole, fp);
            file_item->setData(0, Qt::UserRole + 1, mod_id);
        }
    }

    tree_->expandAll();
}

void ConflictsTab::on_item_double_clicked(QTreeWidgetItem* item, int column) {
    (void)column;
    if (!item) return;

    QString file_path = item->data(0, Qt::UserRole).toString();
    if (file_path.isEmpty()) return;

    QString mod_id = item->data(0, Qt::UserRole + 1).toString();
    if (mod_id.isEmpty()) return;

    emit file_open_requested(mod_id, file_path);
}

void ConflictsTab::on_custom_context_menu(const QPoint& pos) {
    auto* item = tree_->itemAt(pos);
    if (!item) return;

    // Only show merge action on file-level items (leaf nodes with UserRole data)
    QString file_path = item->data(0, Qt::UserRole).toString();
    if (file_path.isEmpty()) return;

    context_file_path_ = file_path;

    QMenu menu(this);
    auto* merge_action = menu.addAction(tr("Merge in ImageDiff"));
    connect(merge_action, &QAction::triggered,
            this, &ConflictsTab::on_merge_in_imagediff);
    menu.exec(tree_->viewport()->mapToGlobal(pos));
}

void ConflictsTab::on_merge_in_imagediff() {
    if (context_file_path_.isEmpty()) return;
    emit image_diff_requested(context_file_path_);
}

}  // namespace ui
