#pragma once

#include <QSet>
#include <QWidget>

class QTreeWidget;
class QTreeWidgetItem;

namespace ui {

// MO2-style category filter panel for the mod list: a checkable category tree
// (from engine::Category::Factory) plus Clear / Edit... buttons. Checking any
// category narrows the mod list to mods carrying at least one checked
// category id (OR semantics, MO2 parity); no checked categories = no category
// filter. The panel is hidden by default and toggled by the << / >> button in
// the ModFilterBar.
//
// The tree is rebuilt from the factory on construction and again on the first
// show (in case plugins registered categories after startup); the checked
// state survives panel hide/show cycles.
class CategoryFilterPanel : public QWidget {
  Q_OBJECT
public:
  explicit CategoryFilterPanel(QWidget *parent = nullptr);

  // Rebuilds the tree from engine::Category::Factory::instance(). Clears the
  // checked state (call only when the tree is empty or a reset is wanted).
  void rebuild();

  // Category ids currently checked (recursive walk of the tree).
  [[nodiscard]] QSet<int> checked_category_ids() const;
  // Tick exactly `ids`, clearing everything else. Ids with no matching item -
  // the category was deleted, or the owning plugin has not registered it yet
  // - are dropped silently, so a stale saved set can never wedge the panel.
  // Does not emit category_filter_changed: this is a restore, not a user
  // action, so the caller decides whether to re-apply the mod filter.
  void set_checked_category_ids(const QSet<int> &ids);
  // True when at least one category is checked (an active filter).
  [[nodiscard]] bool has_active_filter() const {
    return !checked_category_ids().isEmpty();
  }

  // Settings > Mod List > "Remember filter settings": write the ticked ids to
  // the settings store, and re-tick them on a later panel. The panel owns this
  // because it is the only place that knows which ids are still real. Both are
  // no-ops while the setting is off, so an untouched install never persists
  // anything and never re-ticks anything.
  void save_state();
  void restore_state();

  // Unchecks every category and emits category_filter_changed.
  void clear_filter();

signals:
  // Emitted whenever the checked set changes (checkbox toggle or Clear).
  void category_filter_changed();
  // Emitted when the user clicks "Edit...". The category editor dialog is
  // tracked by Workspace-l36.4; the controller connects this to it.
  void edit_categories_clicked();

protected:
  void showEvent(QShowEvent *event) override;

private:
  void add_children(QTreeWidgetItem *root, int parent_id);
  void apply_checked(QTreeWidgetItem *node, const QSet<int> &ids);
  void collect_checked(QTreeWidgetItem *node, QSet<int> &out) const;
  void set_all_unchecked(QTreeWidgetItem *node);
  void on_item_changed(QTreeWidgetItem *item, int column);

  QTreeWidget *tree_ = nullptr;
  bool rebuilding_   = false;
};

}  // namespace ui