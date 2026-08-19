#include "ui/widgets/category_filter_panel.h"

#include "engine/pipeline/plugin_host/category_factory.h"

#include <QHBoxLayout>
#include <QPushButton>
#include <QShowEvent>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <vector>

namespace ui {

CategoryFilterPanel::CategoryFilterPanel(QWidget* parent)
    : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 2, 4, 2);
    layout->setSpacing(4);

    tree_ = new QTreeWidget(this);
    tree_->setHeaderHidden(true);
    tree_->setMinimumWidth(160);
    layout->addWidget(tree_, 1);

    auto* buttons = new QHBoxLayout();
    buttons->setSpacing(4);
    auto* clear_btn = new QPushButton(tr("Clear"), this);
    clear_btn->setToolTip(tr("Clear the category filter"));
    buttons->addWidget(clear_btn);
    buttons->addStretch(1);
    auto* edit_btn = new QPushButton(tr("Edit..."), this);
    edit_btn->setToolTip(tr("Edit the category list"));
    buttons->addWidget(edit_btn);
    layout->addLayout(buttons);

    connect(tree_, &QTreeWidget::itemChanged, this,
            &CategoryFilterPanel::on_item_changed);
    connect(clear_btn, &QPushButton::clicked, this,
            &CategoryFilterPanel::clear_filter);
    connect(edit_btn, &QPushButton::clicked, this,
            &CategoryFilterPanel::edit_categories_clicked);

    rebuild();
}

void CategoryFilterPanel::rebuild() {
    rebuilding_ = true;
    tree_->clear();
    add_children(tree_->invisibleRootItem(), 0);
    tree_->expandAll();
    rebuilding_ = false;
}

void CategoryFilterPanel::add_children(QTreeWidgetItem* root, int parent_id) {
    const auto& cats = engine::CategoryFactory::instance().categories();

    // Children of `parent_id`, sorted by name (case-insensitive) so the panel
    // reads like MO2's alphabetized category list rather than raw id order.
    std::vector<const engine::CategoryFactory::Category*> children;
    for (const auto& [id, cat] : cats) {
        if (cat.parent_id == parent_id)
            children.push_back(&cat);
    }
    std::sort(children.begin(), children.end(),
              [](const auto* a, const auto* b) {
                  return QString::fromStdString(a->name)
                             .compare(QString::fromStdString(b->name),
                                      Qt::CaseInsensitive) < 0;
              });

    for (const auto* cat : children) {
        auto* item = new QTreeWidgetItem(root);
        item->setText(0, QString::fromStdString(cat->name));
        item->setData(0, Qt::UserRole, cat->id);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, Qt::Unchecked);
        add_children(item, cat->id);
    }
}

QSet<int> CategoryFilterPanel::checked_category_ids() const {
    QSet<int> out;
    collect_checked(tree_->invisibleRootItem(), out);
    return out;
}

void CategoryFilterPanel::collect_checked(QTreeWidgetItem* node,
                                          QSet<int>& out) const {
    for (int i = 0; i < node->childCount(); ++i) {
        QTreeWidgetItem* child = node->child(i);
        if (child->checkState(0) == Qt::Checked)
            out.insert(child->data(0, Qt::UserRole).toInt());
        collect_checked(child, out);
    }
}

void CategoryFilterPanel::clear_filter() {
    rebuilding_ = true;
    set_all_unchecked(tree_->invisibleRootItem());
    rebuilding_ = false;
    emit category_filter_changed();
}

void CategoryFilterPanel::set_all_unchecked(QTreeWidgetItem* node) {
    for (int i = 0; i < node->childCount(); ++i) {
        QTreeWidgetItem* child = node->child(i);
        child->setCheckState(0, Qt::Unchecked);
        set_all_unchecked(child);
    }
}

void CategoryFilterPanel::on_item_changed(QTreeWidgetItem* item, int column) {
    Q_UNUSED(item)
    Q_UNUSED(column)
    if (rebuilding_)
        return;
    emit category_filter_changed();
}

void CategoryFilterPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    // Plugins register categories at load time (before the UI is built), but
    // rebuild on the first show anyway so late registrations appear. The
    // checked state survives hide/show cycles (rebuild only when empty).
    if (tree_->topLevelItemCount() == 0)
        rebuild();
}

}  // namespace ui