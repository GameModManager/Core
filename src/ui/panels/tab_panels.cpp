#include "ui/panels/tab_panels.h"

#include <QAction>
#include <QApplication>
#include <QColor>
#include <QDesktopServices>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QPalette>
#include <QProgressBar>
#include <QSet>
#include <QStyle>
#include <QTableWidget>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include "ui/widgets/mod_list_model.h"

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
        case DownloadState::Complete:    return QCoreApplication::translate("DownloadsTab", "Install");
        case DownloadState::Installed:   return QCoreApplication::translate("DownloadsTab", "Installed");
        case DownloadState::Failed:      return QCoreApplication::translate("DownloadsTab", "Failed");
    }
    return QCoreApplication::translate("DownloadsTab", "Unknown");
}

// --- PluginsTab ---
PluginsTab::PluginsTab(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    table_ = make_table(3, {tr("Plugin"), tr("Status"), tr("Masters")}, this);
    layout->addWidget(table_);
}

// --- ArchivesTab ---
ArchivesTab::ArchivesTab(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    table_ = make_table(3, {tr("Archive"), tr("Size"), tr("Priority")}, this);
    layout->addWidget(table_);
}

// --- DataTab ---
DataTab::DataTab(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    table_ = make_table(3, {tr("Path"), tr("Size"), tr("Mod")}, this);
    layout->addWidget(table_);
}

// --- SavesTab ---
SavesTab::SavesTab(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    table_ = make_table(3, {tr("Save"), tr("Date"), tr("Size")}, this);
    layout->addWidget(table_);
}

// --- DownloadsTab ---
DownloadsTab::DownloadsTab(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    table_ = make_table(4, {tr("Name"), tr("Source"), tr("Status"), tr("Size")}, this);
    layout->addWidget(table_);

    connect(table_, &QTableWidget::cellDoubleClicked,
            this, &DownloadsTab::on_cell_double_clicked);
}

void DownloadsTab::add_download(const std::string& id, const std::string& name,
                                 const std::string& source,
                                 const std::filesystem::path& file_path) {
    auto [it, inserted] = downloads_.emplace(id, DownloadEntry{});
    if (!inserted) return;  // already exists

    auto& entry = it->second;
    entry.row = next_row_++;
    entry.file_path = file_path;
    entry.state = DownloadState::Downloading;

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
    static DownloadEntry null_entry{-1, {}, DownloadState::Downloading, 0,
                                    nullptr, nullptr, nullptr, nullptr};
    auto it = downloads_.find(id);
    if (it != downloads_.end()) return it->second;
    return null_entry;
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
                                           const QColor& bg) {
    auto& entry = entry_for(id);
    if (entry.row < 0) return;

    // Remove the progress bar widget
    table_->removeCellWidget(entry.row, 2);
    entry.progress_bar = nullptr;

    // Replace with a centered QTableWidgetItem
    auto* item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    item->setTextAlignment(Qt::AlignCenter);
    if (bg.isValid()) {
        item->setBackground(bg);
        item->setForeground(Qt::white);
    }
    table_->setItem(entry.row, 2, item);
}

void DownloadsTab::mark_complete(const std::string& id, bool success) {
    auto& entry = entry_for(id);
    if (entry.row < 0) return;

    entry.state = success ? DownloadState::Complete : DownloadState::Failed;
    QColor bg = success ? QColor("#4CAF50") : QColor("#f44336");
    replace_bar_with_label(id, state_label(entry.state), bg);

    // Show final archive size
    if (entry.total_size > 0) {
        entry.size_item->setText(format_size(entry.total_size));
    }
}

void DownloadsTab::mark_installed(const std::string& id) {
    auto& entry = entry_for(id);
    if (entry.row < 0) return;

    entry.state = DownloadState::Installed;
    replace_bar_with_label(id, tr("Installed"), QColor());

    // Show final archive size
    if (entry.total_size > 0) {
        entry.size_item->setText(format_size(entry.total_size));
    }
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
                    emit install_requested(id, entry.file_path);
                }
            }
            return;
        }
    }
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

            QColor bg;
            if (state == DownloadState::Complete) bg = QColor("#4CAF50");
            else if (state == DownloadState::Failed) bg = QColor("#f44336");
            auto* item = new QTableWidgetItem(state_label(state));
            item->setFlags(item->flags() & ~Qt::ItemIsEditable);
            item->setTextAlignment(Qt::AlignCenter);
            if (bg.isValid()) {
                item->setBackground(bg);
                item->setForeground(Qt::white);
            }
            table_->setItem(entry.row, 2, item);
        }

        table_->resizeRowToContents(entry.row);
    }
}

// --- MIME icon helpers ---
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

}  // anonymous namespace

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

    // Build a set of enabled mod IDs — disabled mods have no influence
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
