#pragma once

#include "engine/filter/filter_combine.h"

#include <QSet>
#include <QWidget>

#include <utility>
#include <vector>

class QButtonGroup;
class QCheckBox;
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
//
// Above the tree sit the special (non-category) filters and below it the
// AND/OR mode, mirroring MO2's FilterList, which carries both in its criteria
// flyout.
class CategoryFilterPanel : public QWidget {
  Q_OBJECT
public:
  // The special filters MO2 offers as first-class criteria
  // (CategoryFactory::SpecialCategories) that the mod list already has data
  // for. "Update available" is deliberately absent: nothing in the mod list
  // records a newer version for a mod, so a filter on it would narrow to
  // nothing rather than to anything.
  enum class Special { Active, Conflict, HiddenFiles };

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

  // Special filters currently ticked. A tick is a filter criterion in its own
  // right, exactly like a checked category: several ticks match a mod when it
  // satisfies any of them (OR within the special group, MO2's behaviour for
  // the special rows), and the mode below then joins the special group with
  // the other filters.
  [[nodiscard]] QSet<Special> checked_specials() const;
  // True when at least one special filter is ticked.
  [[nodiscard]] bool has_active_special() const {
    return !checked_specials().isEmpty();
  }

  // How the filters combine: And keeps a mod only when every ticked filter
  // matches it, Or when any one does (MO2 FilterAnd / FilterOr). And is the
  // default, and the only mode GMM persisted settings can describe.
  [[nodiscard]] engine::filter::Mode filter_mode() const;
  void set_filter_mode(engine::filter::Mode mode);

signals:
  // Emitted whenever the checked set changes (checkbox toggle or Clear).
  void category_filter_changed();
  // Emitted when a special filter or the AND/OR mode changes. Separate from
  // category_filter_changed so the controller can tell a category tick (which
  // the "Remember filter settings" persistence hangs off) from the rest.
  void filters_changed();
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
  // One entry per Special, paired with its checkbox so the enum value never
  // has to be recovered from an index.
  std::vector<std::pair<Special, QCheckBox *>> special_boxes_;
  // And/Or radios, button 0 = And (the default) and button 1 = Or.
  QButtonGroup *mode_group_ = nullptr;
  bool rebuilding_          = false;
};

}  // namespace ui