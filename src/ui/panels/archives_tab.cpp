#include "ui/panels/archives_tab.h"

#include <QAbstractItemView>
#include <QSize>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace ui {

// --- ArchivesTab ---
ArchivesTab::ArchivesTab(QWidget *parent) : QWidget(parent) {
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  tree_ = new QTreeWidget(this);
  tree_->setWhatsThis(
      tr("The archives this instance draws files from, and what is inside them."));
  tree_->setColumnCount(1);
  tree_->setHeaderHidden(true);
  tree_->setRootIsDecorated(false);
  tree_->setAlternatingRowColors(true);
  tree_->setSelectionMode(QAbstractItemView::SingleSelection);
  tree_->setIconSize(QSize(16, 16));
  tree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  layout->addWidget(tree_, 1);
}

void ArchivesTab::set_archives(const QStringList &names, const QStringList &missing) {
  tree_->clear();
  for (const auto &name : names) {
    auto *item = new QTreeWidgetItem(tree_);
    item->setText(0, name);
    const bool gone = missing.contains(name);
    item->setToolTip(0, gone ? tr("%1: not found on disk").arg(name)
                             : tr("%1: present on disk").arg(name));
  }
  tree_->resizeColumnToContents(0);
}

}  // namespace ui