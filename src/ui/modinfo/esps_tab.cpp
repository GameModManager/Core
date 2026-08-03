#include "ui/modinfo/esps_tab.h"

#include <QDirIterator>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

namespace ui {

EspsTab::EspsTab(QWidget* parent) : ModInfoTab(parent) {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    auto make_group = [this](const QString& title) {
        auto* group = new QWidget(this);
        auto* v = new QVBoxLayout(group);
        v->setContentsMargins(0, 0, 0, 0);
        auto* label = new QLabel(title, group);
        label->setStyleSheet(QStringLiteral("font-weight: bold;"));
        v->addWidget(label);
        auto* list = new QListWidget(group);
        v->addWidget(list, 1);
        return std::make_pair(group, list);
    };

    auto [active_group, active_list] = make_group(tr("Active"));
    active_list_ = active_list;
    layout->addWidget(active_group, 1);

    auto* middle = new QVBoxLayout();
    auto* activate_btn = new QPushButton(QChar(0x2192), this);  // →
    activate_btn->setToolTip(tr("Activate (move to the Data root)"));
    auto* deactivate_btn = new QPushButton(QChar(0x2190), this);  // ←
    deactivate_btn->setToolTip(tr("Deactivate (move to optional/)"));
    middle->addStretch(1);
    middle->addWidget(activate_btn);
    middle->addWidget(deactivate_btn);
    middle->addStretch(1);
    layout->addLayout(middle);

    auto [inactive_group, inactive_list] = make_group(tr("Inactive"));
    inactive_list_ = inactive_list;
    layout->addWidget(inactive_group, 1);

    connect(activate_btn, &QPushButton::clicked, this, &EspsTab::on_activate);
    connect(deactivate_btn, &QPushButton::clicked, this, &EspsTab::on_deactivate);
}

EspsTab::~EspsTab() = default;

void EspsTab::set_mod(const ModInfoData& data) {
    esps_.clear();
    data_dir_ = data.mod_dir.absolutePath();

    const QDir root(data_dir_);
    if (root.exists()) {
        QDirIterator it(root.absolutePath(), QDir::Files | QDir::NoDotAndDotDot,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString full = it.next();
            if (!full.endsWith(QStringLiteral(".esp"), Qt::CaseInsensitive) &&
                !full.endsWith(QStringLiteral(".esm"), Qt::CaseInsensitive) &&
                !full.endsWith(QStringLiteral(".esl"), Qt::CaseInsensitive)) {
                continue;
            }
            const QString rel = root.relativeFilePath(full);
            Esp e;
            e.root_path = root.absolutePath();
            e.filename = QFileInfo(full).fileName();
            if (rel.contains(QLatin1Char('/')) ||
                rel.contains(QLatin1Char('\\'))) {
                e.inactive_path = rel;
            } else {
                e.active_path = rel;
            }
            esps_.push_back(std::move(e));
        }
    }

    std::sort(esps_.begin(), esps_.end(),
              [](const Esp& a, const Esp& b) { return a.filename < b.filename; });
    set_has_data(!esps_.empty());
    repopulate({}, {});
}

void EspsTab::repopulate(const QString& focus_active,
                         const QString& focus_inactive) {
    active_list_->clear();
    inactive_list_->clear();
    for (const auto& e : esps_) {
        auto* item = new QListWidgetItem(e.filename,
                                         e.is_active() ? active_list_
                                                       : inactive_list_);
        item->setData(Qt::UserRole, e.is_active() ? e.active_path
                                                  : e.inactive_path);
    }

    const auto focus = [](QListWidget* list, const QString& path) {
        if (path.isEmpty()) return;
        for (int i = 0; i < list->count(); ++i) {
            if (list->item(i)->data(Qt::UserRole).toString() == path) {
                list->setCurrentRow(i);
                return;
            }
        }
    };
    focus(active_list_, focus_active);
    focus(inactive_list_, focus_inactive);

    active_list_->setEnabled(!esps_.empty());
    inactive_list_->setEnabled(!esps_.empty());
}

int EspsTab::index_of(const QListWidget* list, const QString& path) const {
    for (int i = 0; i < static_cast<int>(esps_.size()); ++i) {
        const auto& e = esps_[static_cast<size_t>(i)];
        const QString rel = e.is_active() ? e.active_path : e.inactive_path;
        if (rel == path) return i;
    }
    return -1;
}

void EspsTab::on_activate() {
    auto* item = inactive_list_->currentItem();
    if (!item) return;
    const QString rel = item->data(Qt::UserRole).toString();
    const int i = index_of(inactive_list_, rel);
    if (i < 0) return;

    Esp& e = esps_[static_cast<size_t>(i)];
    QDir root(e.root_path);

    QString new_name = e.filename;
    while (root.exists(new_name)) {
        bool ok = false;
        const QString entered = QInputDialog::getText(
            this, tr("File Exists"),
            tr("A file with that name exists. Please enter a new one:"),
            QLineEdit::Normal, e.filename, &ok);
        if (!ok) return;
        if (!entered.trimmed().isEmpty()) new_name = entered.trimmed();
    }

    if (!root.rename(e.inactive_path, new_name)) {
        QMessageBox::warning(this, tr("Activate Plugin"),
                             tr("Failed to move \"%1\" to the Data root.")
                                 .arg(e.filename));
        return;
    }

    e.active_path = new_name;
    e.inactive_path.clear();
    repopulate(new_name, rel);
}

void EspsTab::on_deactivate() {
    auto* item = active_list_->currentItem();
    if (!item) return;
    const QString rel = item->data(Qt::UserRole).toString();
    const int i = index_of(active_list_, rel);
    if (i < 0) return;

    Esp& e = esps_[static_cast<size_t>(i)];
    QDir root(e.root_path);

    QString new_name = e.inactive_path;
    if (new_name.isEmpty()) {
        if (!root.exists(QStringLiteral("optional"))) {
            if (!root.mkdir(QStringLiteral("optional"))) {
                QMessageBox::warning(this, tr("Deactivate Plugin"),
                                     tr("Failed to create \"optional\"."));
                return;
            }
        }
        new_name = QStringLiteral("optional") + QLatin1Char('/') + e.filename;
    }

    if (!root.rename(e.active_path, new_name)) {
        QMessageBox::warning(this, tr("Deactivate Plugin"),
                             tr("Failed to move \"%1\" to optional/.")
                                 .arg(e.filename));
        return;
    }

    e.inactive_path = new_name;
    e.active_path.clear();
    repopulate(rel, new_name);
}

}  // namespace ui
