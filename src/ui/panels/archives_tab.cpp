#include "ui/panels/archives_tab.h"

#include <QAbstractItemView>
#include <QSize>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace ui {

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

}  // namespace ui
