#include "ui/modinfo/conflicts_tab.h"

#include "engine/core/util/fs_utils.h"

#include <QApplication>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <array>

namespace ui {

namespace {
QString strip_data_prefix(const ModInfoData& data, const QString& rel) {
    if (!data.data_subpath.isEmpty() &&
        rel.startsWith(data.data_subpath + QLatin1Char('/')))
        return rel.mid(data.data_subpath.length() + 1);
    return rel;
}

int extreme_priority(const std::vector<std::pair<QString, int>>& owners,
                     bool reversed) {
    int best = owners.front().second;
    for (const auto& o : owners)
        best = reversed ? std::min(best, o.second) : std::max(best, o.second);
    return best;
}
}  // namespace

ConflictsInfoTab::ConflictsInfoTab(QWidget* parent) : ModInfoTab(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    auto make_group = [this, layout](const QString& title) {
        auto* group = new QWidget(this);
        auto* v = new QVBoxLayout(group);
        v->setContentsMargins(0, 0, 0, 0);
        auto* header = new QHBoxLayout();
        auto* label = new QLabel(title, group);
        QFont f = label->font();
        f.setBold(true);
        label->setFont(f);
        auto* count = new QLabel(group);
        auto* filter = new QLineEdit(group);
        filter->setPlaceholderText(tr("Filter..."));
        header->addWidget(label);
        header->addWidget(count);
        header->addWidget(filter, 1);
        v->addLayout(header);
        auto* list = new QTreeWidget(group);
        list->setHeaderLabels({tr("File"), tr("Provider")});
        list->setRootIsDecorated(false);
        list->setUniformRowHeights(true);
        list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        list->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        list->setContextMenuPolicy(Qt::CustomContextMenu);
        v->addWidget(list, 1);
        layout->addWidget(group, 1);
        return std::make_tuple(group, list, filter, count);
    };

    auto [w, w_list, w_filter, w_count] = make_group(tr("Winning files"));
    wins_.list = w_list;
    wins_.filter = w_filter;
    wins_.count = w_count;

    auto [l, l_list, l_filter, l_count] = make_group(tr("Losing files"));
    loses_.list = l_list;
    loses_.filter = l_filter;
    loses_.count = l_count;

    auto [n, n_list, n_filter, n_count] = make_group(tr("Non conflicting files"));
    no_conflict_.list = n_list;
    no_conflict_.filter = n_filter;
    no_conflict_.count = n_count;

    for (auto* group : {&wins_, &loses_, &no_conflict_}) {
        connect(group->filter, &QLineEdit::textChanged, this,
                [this, group]() { apply_filter(*group); });
        connect(group->list, &QTreeWidget::customContextMenuRequested, this,
                [this, group](const QPoint& pos) { show_menu(*group, pos); });
        connect(group->list, &QTreeWidget::itemDoubleClicked, this,
                [this, group](QTreeWidgetItem* item, int) {
                    if (!item) return;
                    const int row = group->list->indexOfTopLevelItem(item);
                    if (row < 0 ||
                        row >= static_cast<int>(group->files.size()))
                        return;
                    QDesktopServices::openUrl(QUrl::fromLocalFile(
                        group->files[static_cast<size_t>(row)].abs_path));
                });
    }
}

ConflictsInfoTab::~ConflictsInfoTab() = default;

void ConflictsInfoTab::set_mod(const ModInfoData& data) {
    wins_.files.clear();
    loses_.files.clear();
    no_conflict_.files.clear();

    const QString mod_root = data.mod_dir.absolutePath();
    for (const auto& [path, owners] : data.conflicts) {
        bool is_owner = false;
        for (const auto& [owner, prio] : owners)
            if (owner == data.id) { is_owner = true; break; }
        if (!is_owner) continue;

        const QString rel = path;
        File f;
        f.rel_path = rel;
        f.display = strip_data_prefix(data, rel);
        f.abs_path = mod_root + QLatin1Char('/') + rel;

        QStringList provider_names;
        provider_names.reserve(static_cast<int>(owners.size()));
        for (const auto& [owner, prio] : owners)
            provider_names << owner;
        f.provider = provider_names.join(QStringLiteral(", "));

        if (owners.size() <= 1) {
            no_conflict_.files.push_back(std::move(f));
            continue;
        }
        const int best = extreme_priority(owners, data.conflict_reversed);
        bool won = false;
        for (const auto& [owner, prio] : owners) {
            if (prio == best) { won = (owner == data.id); break; }
        }
        f.won = won;
        (won ? wins_ : loses_).files.push_back(std::move(f));
    }

    std::array<Group*, 3> groups = {&wins_, &loses_, &no_conflict_};
    for (auto* group : groups) rebuild(*group);

    set_has_data(!wins_.files.empty() || !loses_.files.empty());
}

void ConflictsInfoTab::rebuild(Group& group) {
    group.list->clear();
    for (const auto& f : group.files) {
        auto* item = new QTreeWidgetItem(group.list);
        item->setText(0, f.display);
        item->setText(1, f.provider);
        item->setToolTip(0, f.abs_path);
        item->setToolTip(1, f.abs_path);
    }
    group.count->setText(QStringLiteral("%1").arg(group.files.size()));
    group.list->setEnabled(!group.files.empty());
    group.filter->setEnabled(!group.files.empty());
    apply_filter(group);
}

void ConflictsInfoTab::apply_filter(Group& group) {
    const QString needle = group.filter->text().trimmed();
    for (int row = 0; row < group.list->topLevelItemCount(); ++row) {
        auto* item = group.list->topLevelItem(row);
        const bool visible =
            needle.isEmpty() ||
            item->text(0).contains(needle, Qt::CaseInsensitive) ||
            item->text(1).contains(needle, Qt::CaseInsensitive);
        item->setHidden(!visible);
    }
}

QString ConflictsInfoTab::selected_path(Group& group) const {
    auto* item = group.list->currentItem();
    if (!item) return {};
    const int row = group.list->indexOfTopLevelItem(item);
    if (row < 0 || row >= static_cast<int>(group.files.size())) return {};
    return group.files[static_cast<size_t>(row)].abs_path;
}

void ConflictsInfoTab::show_menu(Group& group, const QPoint& pos) {
    const QString abs = selected_path(group);
    if (abs.isEmpty()) return;

    QMenu menu(this);
    const bool hidden = engine::is_hidden_file(abs.toStdString());

    auto* open = menu.addAction(tr("&Open"));
    QObject::connect(open, &QAction::triggered, this, [abs]() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(abs));
    });

    auto* explore = menu.addAction(tr("Open in &Explorer"));
    QObject::connect(explore, &QAction::triggered, this, [abs]() {
        QDesktopServices::openUrl(
            QUrl::fromLocalFile(QFileInfo(abs).absolutePath()));
    });

    menu.addSeparator();

    auto* hide = menu.addAction(hidden ? tr("&Unhide") : tr("&Hide"));
    QObject::connect(hide, &QAction::triggered, this,
                     [this, &group, hidden]() { on_hide(group, !hidden); });

    menu.exec(group.list->viewport()->mapToGlobal(pos));
}

void ConflictsInfoTab::on_hide(Group& group, bool hide) {
    const QString abs = selected_path(group);
    if (abs.isEmpty()) return;
    if (!current().hide_file || !current().refresh_conflicts) return;

    if (current().hide_file(abs, hide)) {
        current().refresh_conflicts();
    }
}

}  // namespace ui
