#pragma once

#include "ui/widgets/mod_list_model.h"
#include <QSortFilterProxyModel>
#include <Qt>

namespace ui {

// Presentation-only sort/filter proxy between ModView and ModList.
// Source order, priorities, separator bands, folds, and drag semantics are
// preserved: the proxy only permutes ordinary sibling mods inside a band.
class ModListSortProxy : public QSortFilterProxyModel {
  Q_OBJECT
public:
  explicit ModListSortProxy(ModList *source, QObject *parent = nullptr);

  // Display order the user requested (may differ from the base sort order,
  // which is always Ascending because ranks already encode direction).
  Qt::SortOrder display_order() const { return display_order_; }
  int sort_column() const { return sort_column_; }

  void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;

  bool filterAcceptsRow(int source_row, const QModelIndex &source_parent) const override;

  // Drag guard: internal mod moves only while sorted by Priority ascending.
  bool canDropMimeData(const QMimeData *data, Qt::DropAction action, int row,
                       int column, const QModelIndex &parent) const override;
  bool dropMimeData(const QMimeData *data, Qt::DropAction action, int row,
                    int column, const QModelIndex &parent) override;

  // Rebuild ranks after structural changes.
  void rebuild_ranks();

  // Access to the source model.
  ModList *mod_list() const { return mod_list_; }

signals:
  void drag_refused();

private:
  void rebuild_rank_for(int column, Qt::SortOrder order);

  ModList *mod_list_ = nullptr;
  int sort_column_   = ModList::Priority;  // placeholder, set properly
  Qt::SortOrder display_order_ = Qt::AscendingOrder;
  QVector<int> rank_;  // rank_[source_row] = display position
};

}  // namespace ui
