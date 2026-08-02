#include "ui/panels/tab_panels.h"
#include "ui/settings/settings.h"

#include <algorithm>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QDesktopServices>#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPalette>
#include <QPainter>
#include <QPixmap>
#include <QProgressBar>
#include <QSet>
#include <QShowEvent>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include "ui/widgets/column_toggle_header.h"
#include "ui/widgets/mod_list_model.h"

#include "engine/log/logger.h"
#include "engine/fs_utils.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>

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
        QStyle* st = QApplication::style();
        folder = st->standardIcon(QStyle::SP_DirIcon);
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

// Recursively sort a tree so directories come first, then alphabetical.
void sort_dirs_first(QTreeWidgetItem* parent) {
    std::vector<QTreeWidgetItem*> children;
    children.reserve(parent->childCount());
    for (int i = 0; i < parent->childCount(); ++i)
        children.push_back(parent->child(i));

    std::sort(children.begin(), children.end(),
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

}  // anonymous namespace

// --- PluginsTab ---

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

// MO2-style status emblems for the plugin list Flags column. MO2 renders a
// horizontal stack of small emblem icons in that column (PluginList::iconData
// + GenericIconDelegate); a cell can only carry one icon, so the selected
// emblems are laid out into a single pixmap, the same trick ModListModel uses
// for conflict icons. Emblem names mirror MO2's iconData() tokens.
static QIcon plugin_flag_emblem_icon(const QStringList& marks) {
    constexpr int kSize = 16;
    constexpr int kGap = 4;

    static const QString icon_dir =
        QCoreApplication::applicationDirPath() + "/../resources/icons/";
    static const QIcon warning(icon_dir + "plugin-warning.png");
    static const QIcon awaiting(icon_dir + "plugin-awaiting.png");
    static const QIcon run(icon_dir + "plugin-medium.png");

    QVector<QIcon> icons;
    icons.reserve(marks.size());
    for (const QString& m : marks) {
        if (m == QLatin1String("warning"))
            icons.push_back(warning);
        else if (m == QLatin1String("awaiting"))
            icons.push_back(awaiting);
        else if (m == QLatin1String("run"))
            icons.push_back(run);
    }
    if (icons.isEmpty()) return {};

    const int total = static_cast<int>(icons.size()) * kSize +
                      std::max(0, static_cast<int>(icons.size()) - 1) * kGap;
    QPixmap pix(total, kSize);
    pix.fill(Qt::transparent);
    QPainter p(&pix);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    int x = 0;
    for (const QIcon& icon : icons) {
        p.drawPixmap(x, 0, kSize, kSize, icon.pixmap(kSize, kSize));
        x += kSize + kGap;
    }
    p.end();
    return QIcon(pix);
}

QTableWidget* PluginsTab::table() const {
    return table_;
}

PluginsTab::PluginsTab(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    table_ = new PluginTable(0, 4, this);
    table_->setHorizontalHeaderLabels(
        {tr("Plugin Name"), tr("Flags"), tr("Priority"), tr("Mod Index")});
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->verticalHeader()->setVisible(false);
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
    layout->addWidget(table_);
}

void PluginsTab::set_plugins(const std::vector<engine::GamePlugin>& plugins) {
    syncing_ = true;
    table_->setRowCount(0);
    names_.clear();
    names_.reserve(plugins.size());
    table_->setRowCount(static_cast<int>(plugins.size()));

    const QColor missing_color(0xB0, 0x30, 0x30);
    const QColor fixed_color(Qt::gray);

    for (int i = 0; i < static_cast<int>(plugins.size()); ++i) {
        const auto& p = plugins[static_cast<size_t>(i)];
        names_.push_back(p.name);

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
            nf |= Qt::ItemIsUserCheckable | Qt::ItemIsDragEnabled;
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
        QString tip;
        if (!p.owner_mod.empty())
            tip += tr("Installed by: %1\n").arg(QString::fromStdString(p.owner_mod));
        if (!p.masters.empty()) {
            QStringList m;
            for (const auto& master : p.masters) m << QString::fromStdString(master);
            tip += tr("Masters: %1\n").arg(m.join(", "));
        }
        if (p.missing_master) tip += tr("A required master is not installed.");
        if (!tip.isEmpty()) name->setToolTip(tip.trimmed());
        table_->setItem(i, 0, name);

        // Column 1: status emblems (MO2's PluginList::iconData parity). MO2
        // shows: warning (missing masters / problems), awaiting (ESL-flagged
        // but not .esl), run (medium/ESH). It never icons the plugin *type*
        // here — that's the name font above.
        QStringList marks;
        QStringList reasons;
        if (p.missing_master) {
            marks << "warning";
            reasons << tr("A required master is not installed");
        }
        if (p.is_light_flagged && !p.has_light_ext) {
            marks << "awaiting";
            reasons << tr("Flagged as light (ESL) but does not use the .esl extension");
        }
        if (p.is_medium_flagged) {
            marks << "run";
            reasons << tr("Medium plugin (ESH)");
        }
        if (p.is_medium_flagged && p.is_light_flagged) {
            marks << "warning";
            reasons << tr("Both light and medium flagged — may have mismatched records");
        }
        marks.removeDuplicates();
        auto* flags = new QTableWidgetItem;
        Qt::ItemFlags ff = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        if (!p.force_loaded) ff |= Qt::ItemIsDragEnabled;
        flags->setFlags(ff);
        if (!marks.isEmpty()) {
            flags->setIcon(plugin_flag_emblem_icon(marks));
            flags->setToolTip(reasons.join("\n"));
        }
        table_->setItem(i, 1, flags);

        // Column 2: priority.
        auto* prio = new QTableWidgetItem(QString::number(p.priority));
        Qt::ItemFlags pf = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        if (!p.force_loaded) pf |= Qt::ItemIsDragEnabled;
        prio->setFlags(pf);
        prio->setTextAlignment(Qt::AlignCenter);
        if (p.force_loaded) prio->setForeground(fixed_color);
        table_->setItem(i, 2, prio);

        // Column 3: mod index.
        auto* idx = new QTableWidgetItem(QString::fromStdString(p.mod_index_text));
        Qt::ItemFlags xf = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        if (!p.force_loaded) xf |= Qt::ItemIsDragEnabled;
        idx->setFlags(xf);
        idx->setTextAlignment(Qt::AlignCenter);
        if (p.force_loaded) idx->setForeground(fixed_color);
        table_->setItem(i, 3, idx);
    }
    syncing_ = false;
    apply_highlights();  // rows were rebuilt; re-tint selected-mod/master rows
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
}

void DataTab::clear_content() {
    tree_->clear();
}

void DataTab::show_data(
    const std::unordered_map<std::string, std::vector<std::pair<std::string, int>>>& registry,
    const QVector<ModEntry>& all_mods,
    bool conflict_reversed,
    const std::filesystem::path& mods_dir,
    const std::filesystem::path& game_mods_dir)
{
    tree_->clear();
    if (registry.empty()) return;

    // mod id -> display name lookup
    std::unordered_map<std::string, QString> display_names;
    for (const auto& m : all_mods)
        display_names[m.id.toStdString()] = m.name.isEmpty() ? m.id : m.name;

    struct Row {
        QString path;
        QString source;
        qint64 size = -1;
        int providers = 0;
        QStringList all_sources;
    };
    std::vector<Row> rows;
    rows.reserve(registry.size());

    for (const auto& [path, owners] : registry) {
        if (owners.empty()) continue;

        Row row;
        row.path = QString::fromStdString(path).replace('\\', '/');

        // Winner = the provider that actually takes effect
        auto winner = conflict_reversed
            ? std::min_element(owners.begin(), owners.end(),
                               [](const auto& a, const auto& b) { return a.second < b.second; })
            : std::max_element(owners.begin(), owners.end(),
                               [](const auto& a, const auto& b) { return a.second < b.second; });

        QString winner_id = QString::fromStdString(winner->first);
        if (winner_id == QLatin1String(kOverwriteModId)) {
            row.source = tr("Overwrite");
        } else if (winner_id == QLatin1String(kMergedModId)) {
            row.source = tr("MERGED");
        } else {
            auto it = display_names.find(winner->first);
            row.source = it != display_names.end() ? it->second : winner_id;
        }

        row.providers = static_cast<int>(owners.size());
        for (const auto& [owner, _] : owners) {
            auto it = display_names.find(owner);
            row.all_sources << (it != display_names.end() ? it->second
                                                          : QString::fromStdString(owner));
        }

        // Size of the winning copy: instance mods dir first, then game-native
        std::error_code ec;
        auto sz = std::filesystem::file_size(mods_dir / winner->first / path, ec);
        if (ec && !game_mods_dir.empty()) {
            ec.clear();
            sz = std::filesystem::file_size(game_mods_dir / winner->first / path, ec);
        }
        if (!ec) row.size = static_cast<qint64>(sz);

        rows.push_back(std::move(row));
    }

    // Sorted path order gives naturally grouped tree insertion
    std::sort(rows.begin(), rows.end(),
              [](const Row& a, const Row& b) { return a.path < b.path; });

    engine::Logger::instance().debug("Data tab populated: " +
        std::to_string(rows.size()) + " merged files");

    for (const auto& row : rows) {
        auto parts = row.path.split('/');
        auto* parent = tree_->invisibleRootItem();
        for (int i = 0; i < parts.size() - 1; ++i)
            parent = ensure_child(parent, parts[i], true);

        auto* file_item = ensure_child(parent, parts.last(), false);
        file_item->setIcon(0, icon_for_file(parts.last()));
        file_item->setText(1, row.size >= 0 ? format_size(row.size) : QString());
        file_item->setText(2, row.source);
        file_item->setToolTip(2, row.all_sources.join(", "));
        if (row.providers > 1) {
            file_item->setText(3, QString::number(row.providers));
            file_item->setToolTip(3, row.all_sources.join("\n"));
        }
    }

    sort_dirs_first(tree_->invisibleRootItem());
    tree_->expandToDepth(1);
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
    layout->addLayout(top);

    table_ = make_table(4, {tr("Name"), tr("Source"), tr("Status"), tr("Size")}, this);
    layout->addWidget(table_, 1);

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
}

void DownloadsTab::add_download(const std::string& id, const std::string& name,
                                 const std::string& source,
                                 const std::filesystem::path& file_path,
                                 const std::string& nexus_domain,
                                 int file_id,
                                 const std::string& parent_mod_id) {
    auto [it, inserted] = downloads_.emplace(id, DownloadEntry{});
    if (!inserted) return;  // already exists

    auto& entry = it->second;
    entry.row = next_row_++;
    entry.file_path = file_path;
    entry.state = DownloadState::Downloading;
    entry.nexus_domain = nexus_domain;
    entry.file_id = file_id;
    entry.parent_mod_id = parent_mod_id;

    table_->insertRow(entry.row);

    entry.name_item = new QTableWidgetItem(QString::fromStdString(name));
    entry.name_item->setFlags(entry.name_item->flags() & ~Qt::ItemIsEditable);
    table_->setItem(entry.row, 0, entry.name_item);

    entry.source_item = new QTableWidgetItem(QString::fromStdString(source));
    entry.source_item->setFlags(entry.source_item->flags() & ~Qt::ItemIsEditable);
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

    table_->resizeRowToContents(entry.row);
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
        table_->resizeRowToContents(entry.row);
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

    table_->resizeRowToContents(entry.row);
}

void DownloadsTab::set_file_path(const std::string& id, const std::filesystem::path& path) {
    auto& entry = entry_for(id);
    if (entry.row < 0) return;
    entry.file_path = path;
}

void DownloadsTab::set_downloads_dir(const std::filesystem::path& dir) {
    downloads_dir_ = dir;
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
    if (downloads_dir_.empty()) return;
    // While a download is downloading or paused its partial archive sits in
    // the dir under its final name; don't surface it as a "Complete" entry.
    if (has_active_download()) return;

    static const std::vector<std::string> kArchiveExts = {
        ".zip", ".7z", ".tar", ".rar", ".gz", ".bz2", ".xz", ".fomod"};

    std::error_code ec;
    std::filesystem::directory_iterator it(downloads_dir_, ec);
    if (ec) return;
    for (const auto& entry : it) {
        if (!entry.is_regular_file(ec)) continue;
        const auto path = entry.path();

        std::string lower;
        for (char c : path.extension().string())
            lower.push_back(static_cast<char>(std::tolower(
                static_cast<unsigned char>(c))));
        if (std::find(kArchiveExts.begin(), kArchiveExts.end(), lower) ==
            kArchiveExts.end())
            continue;

        const auto id = path.filename().string();
        if (downloads_.count(id)) continue;

        // Skip archives that already back a tracked entry under a different
        // key (Nexus downloads use "<mod_id>-<file_id>", not the filename).
        bool tracked = false;
        for (const auto& [eid, e] : downloads_) {
            (void)eid;
            if (e.file_path == path) {
                tracked = true;
                break;
            }
        }
        if (tracked) continue;

        add_download(id, path.stem().string(), tr("Manual").toStdString(), path);
        auto& added = downloads_.at(id);
        added.total_size = static_cast<int64_t>(entry.file_size(ec));
        if (ec) added.total_size = 0;
        mark_complete(id, true);
    }
    apply_installed_filter();
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
                    emit install_requested(id, entry.file_path,
                        entry.parent_mod_id.empty() ? "" : "nexus",
                        entry.parent_mod_id, entry.file_id,
                        entry.name_item ? entry.name_item->text().toStdString()
                                        : std::string());
                }
            }
            return;
        }
    }
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
    const std::string id = *found;
    const auto& entry = downloads_.at(id);

    // Theme icons with a standard-icon fallback (matches the mod-list menu
    // and toolbar on non-themed platforms).
    auto icon_for = [](const QString& theme, QStyle::StandardPixmap fallback) -> QIcon {
        QIcon icon = QIcon::fromTheme(theme);
        if (icon.isNull())
            icon = QApplication::style()->standardIcon(fallback);
        return icon;
    };

    QMenu menu(this);

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
            emit install_requested(id, entry.file_path,
                entry.parent_mod_id.empty() ? "" : "nexus",
                entry.parent_mod_id, entry.file_id,
                entry.name_item ? entry.name_item->text().toStdString()
                                : std::string());
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

    auto* nexus_action = menu.addAction(
        icon_for("text-html", QStyle::SP_FileDialogInfoView),
        tr("Open on Nexus"), this, [entry]() {
            QDesktopServices::openUrl(QUrl(QString::fromStdString(
                "https://www.nexusmods.com/" + entry.nexus_domain +
                "/mods/" + entry.parent_mod_id)));
        });
    nexus_action->setEnabled(!entry.nexus_domain.empty() &&
                             !entry.parent_mod_id.empty());

    menu.addSeparator();
    menu.addAction(icon_for("edit-delete", QStyle::SP_TrashIcon),
        tr("Remove"), this, [this, id]() { remove_entry(id); });

    menu.exec(table_->viewport()->mapToGlobal(pos));
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
        entry.row = next_row_++;
        entry.file_path = file_path;
        entry.state = state;
        entry.total_size = total_size;
        entry.parent_mod_id = obj["parent_mod_id"].toString().toStdString();
        entry.file_id = obj["file_id"].toInt();
        entry.nexus_domain = obj["domain"].toString().toStdString();
        entry.category = obj["category"].toString().toStdString();

        table_->insertRow(entry.row);

        entry.name_item = new QTableWidgetItem(QString::fromStdString(name));
        entry.name_item->setFlags(entry.name_item->flags() & ~Qt::ItemIsEditable);
        table_->setItem(entry.row, 0, entry.name_item);

        entry.source_item = new QTableWidgetItem(QString::fromStdString(source));
        entry.source_item->setFlags(entry.source_item->flags() & ~Qt::ItemIsEditable);
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

        table_->resizeRowToContents(entry.row);
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
