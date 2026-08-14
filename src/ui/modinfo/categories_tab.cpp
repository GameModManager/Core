#include "ui/modinfo/categories_tab.h"

#include "engine/mod/meta/categories.h"
#include "engine/mod/meta/mod_meta.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSet>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace ui {

CategoriesTab::CategoriesTab(QWidget* parent) : ModInfoTab(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* header = new QHBoxLayout();
    auto* label = new QLabel(tr("Primary category:"), this);
    header->addWidget(label);
    primary_ = new QComboBox(this);
    header->addWidget(primary_, 1);
    layout->addLayout(header);

    tree_ = new QTreeWidget(this);
    tree_->setHeaderHidden(true);
    layout->addWidget(tree_, 1);

    connect(tree_, &QTreeWidget::itemChanged, this, &CategoriesTab::on_item_changed);
    connect(primary_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int index) {
                if (rebuilding_) return;
                if (index >= 0)
                    primary_id_ = primary_->itemData(index).toInt();
                save_tree();
            });
}

CategoriesTab::~CategoriesTab() = default;

void CategoriesTab::set_mod(const ModInfoData& data) {
    rebuilding_ = true;
    tree_->clear();
    primary_->clear();
    rebuilding_ = false;

    // Category DB lives beside the mods dir (same instance root).
    categories_ = std::make_shared<engine::Categories>(
        engine::Categories::load(current().instance_root.toStdString()));
    const auto& categories = *categories_;
    if (categories.categories().empty()) {
        set_has_data(false);
        return;
    }

    // Mod's internal categories come from MO2's "category" CSV (primary first).
    QSet<int> enabled;
    const QString csv = QString::fromStdString(
        data.load_meta().get("General", "category"));
    int primary = 0;
    const auto parts = csv.split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (!parts.isEmpty()) {
        primary = parts.first().toInt();
        for (const auto& p : parts) enabled.insert(p.toInt());
    }

    // Fall back to the Nexus category mapping when the CSV is empty.
    if (enabled.isEmpty()) {
        const int nexus_id =
            QString::fromStdString(data.load_meta().get("Nexusmods", "nexuscategory"))
                .toInt();
        if (nexus_id > 0) {
            if (const auto* cat = categories.category_for_nexus(nexus_id)) {
                enabled.insert(cat->id);
                primary = cat->id;
            }
        }
    }

    primary_id_ = primary;
    add_children(tree_->invisibleRootItem(), 0);

    // Apply the checked state after the tree exists.
    std::function<void(QTreeWidgetItem*)> apply = [&](QTreeWidgetItem* node) {
        for (int i = 0; i < node->childCount(); ++i) {
            QTreeWidgetItem* child = node->child(i);
            if (enabled.contains(child->data(0, Qt::UserRole).toInt()))
                child->setCheckState(0, Qt::Checked);
            apply(child);
        }
    };
    apply(tree_->invisibleRootItem());
    tree_->expandAll();

    set_has_data(true);
    update_primary();
}

void CategoriesTab::add_children(QTreeWidgetItem* root, int parent_id) {
    if (!categories_) return;
    const auto children = categories_->children_of(parent_id);
    for (const auto* cat : children) {
        auto* item = new QTreeWidgetItem(root);
        item->setText(0, QString::fromStdString(cat->name));
        item->setData(0, Qt::UserRole, cat->id);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, Qt::Unchecked);
        add_children(item, cat->id);
    }
}

void CategoriesTab::update_primary() {
    rebuilding_ = true;
    primary_->clear();
    add_checked(tree_->invisibleRootItem());
    int match = -1;
    for (int i = 0; i < primary_->count(); ++i) {
        if (primary_->itemData(i).toInt() == primary_id_) { match = i; break; }
    }
    primary_->setCurrentIndex(match);
    rebuilding_ = false;
}

void CategoriesTab::add_checked(QTreeWidgetItem* node) {
    for (int i = 0; i < node->childCount(); ++i) {
        QTreeWidgetItem* child = node->child(i);
        if (child->checkState(0) == Qt::Checked) {
            primary_->addItem(child->text(0), child->data(0, Qt::UserRole));
            add_checked(child);
        }
    }
}

void CategoriesTab::save_tree() {
    // Save the whole checked set with the current primary first.
    QList<QVariant> ids;
    std::function<void(QTreeWidgetItem*)> collect = [&](QTreeWidgetItem* node) {
        for (int i = 0; i < node->childCount(); ++i) {
            QTreeWidgetItem* child = node->child(i);
            if (child->checkState(0) == Qt::Checked)
                ids.append(child->data(0, Qt::UserRole));
            collect(child);
        }
    };
    collect(tree_->invisibleRootItem());

    QString csv;
    if (!ids.isEmpty() && ids.contains(primary_id_)) {
        ids.removeAll(primary_id_);
        ids.prepend(primary_id_);
    }
    QStringList parts;
    for (const auto& id : ids) parts << id.toString();
    if (!parts.isEmpty()) csv = parts.join(QLatin1Char(','));

    auto meta = current().load_meta();
    const QString before = QString::fromStdString(meta.get("General", "category"));
    if (csv == before) return;  // nothing changed
    meta.set("General", "category", csv.toStdString());
    current().save_meta(meta);
}

void CategoriesTab::persist() {
    save_tree();
}

void CategoriesTab::on_item_changed(QTreeWidgetItem* item, int column) {
    Q_UNUSED(item)
    Q_UNUSED(column)
    if (rebuilding_) return;

    // Checking a category auto-checks its unchecked ancestors (MO2).
    QTreeWidgetItem* node = item;
    while ((node = node->parent()) != nullptr) {
        if ((node->flags() & Qt::ItemIsUserCheckable) &&
            node->checkState(0) == Qt::Unchecked) {
            node->setCheckState(0, Qt::Checked);
        }
    }

    update_primary();
    save_tree();
}

void CategoriesTab::save_state() {
    save_tree();
}

}  // namespace ui
