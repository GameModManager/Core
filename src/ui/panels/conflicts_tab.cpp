#include "ui/panels/conflicts_tab.h"
#include "ui/panels/panel_utils.h"
#include "ui/widgets/mod_list_model.h"

#include <QAction>
#include <QColor>
#include <QFont>
#include <QMap>
#include <QMenu>
#include <QSet>
#include <QSize>
#include <QStringList>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

namespace ui {

// Find or create a child tree item by name under parent.
static QTreeWidgetItem* ensure_child(QTreeWidgetItem* parent, const QString& name, bool is_dir) {
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
