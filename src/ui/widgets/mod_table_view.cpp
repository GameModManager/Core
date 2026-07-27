#include "ui/widgets/mod_table_view.h"

namespace ui {

ModTableView::ModTableView(QWidget* parent)
    : QTreeView(parent) {
    setRootIsDecorated(false);
    setIndentation(0);
    setAlternatingRowColors(true);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setDragDropMode(QAbstractItemView::InternalMove);
    setDefaultDropAction(Qt::MoveAction);
    setDragEnabled(true);
    setAcceptDrops(true);
    setDropIndicatorShown(true);
}

}  // namespace ui
