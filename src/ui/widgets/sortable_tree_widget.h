#pragma once

#include <QTreeWidget>

class QMimeData;
class QTreeWidgetItem;

namespace ui {

// A QTreeWidget that moves its own items by drag and drop and says when it did.
// A plain subclass, so the whole QTreeWidget item API works unchanged.
//
// MO2 parity: MOBase::SortableTreeWidget (uibase/sortabletreewidget.h) -
// internal-move drag and drop, setLocalMoveOnly() to confine a move to the
// item's own branch, and itemsMoved() once a drop has happened.
//
// No feature logic here: the caller owns the items and reads the signal.
class SortableTreeWidget : public QTreeWidget {
  Q_OBJECT
public:
  explicit SortableTreeWidget(QWidget *parent = nullptr);

  // true: a drop is refused unless every dragged item lands in the branch it
  // came from, so items can only be reordered within a branch. false
  // (the default): items may be moved between branches.
  void set_local_move_only(bool local_only);
  [[nodiscard]] bool local_move_only() const { return local_move_only_; }

signals:
  // Emitted after an accepted drop. Never for a refused drop, so a caller can
  // use it as "something moved" rather than "a drag was attempted".
  void items_moved();

protected:
  bool dropMimeData(QTreeWidgetItem *parent, int index, const QMimeData *data,
                    Qt::DropAction action) override;
  Qt::DropActions supportedDropActions() const override;

private:
  // True when every selected item would stay in `parent` if dropped there.
  // An empty selection counts as staying put: there is nothing to refuse.
  [[nodiscard]] bool drag_stays_in_branch(QTreeWidgetItem *parent) const;

  bool local_move_only_ = false;
};

}  // namespace ui