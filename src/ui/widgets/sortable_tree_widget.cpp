#include "ui/widgets/sortable_tree_widget.h"

#include <QMimeData>

#include <algorithm>

namespace ui {

SortableTreeWidget::SortableTreeWidget(QWidget *parent) : QTreeWidget(parent) {
  // InternalMove: the items being dragged already belong to this widget, so a
  // drop must move them, never duplicate them.
  setDragDropMode(QAbstractItemView::InternalMove);
  setDragEnabled(true);
  setAcceptDrops(true);
  setDropIndicatorShown(true);
  setDefaultDropAction(Qt::MoveAction);
}

void SortableTreeWidget::set_local_move_only(bool local_only) {
  local_move_only_ = local_only;
}

bool SortableTreeWidget::dropMimeData(QTreeWidgetItem *parent, int index,
                                      const QMimeData *data, Qt::DropAction action) {
  if (local_move_only_ && !drag_stays_in_branch(parent)) {
    // Refused: the base is never asked, so nothing moves and nothing is
    // emitted - a caller wired to items_moved() can rely on that.
    return false;
  }

  if (!QTreeWidget::dropMimeData(parent, index, data, action))
    return false;

  emit items_moved();
  return true;
}

Qt::DropActions SortableTreeWidget::supportedDropActions() const {
  // Move only, and spelled out rather than inherited: QAbstractItemView narrows
  // InternalMove to MoveAction for us, but if a caller ever puts this widget in
  // DragDrop mode the base would then also advertise Copy, and a "copy" of an
  // item this widget already owns is a silently duplicated row.
  return QTreeWidget::supportedDropActions() & Qt::MoveAction;
}

bool SortableTreeWidget::drag_stays_in_branch(QTreeWidgetItem *const parent) const {
  const QList<QTreeWidgetItem *> selected = selectedItems();
  return std::none_of(selected.cbegin(), selected.cend(),
                      [parent](const QTreeWidgetItem *item) {
                        return item->parent() != parent;
                      });
}

}  // namespace ui