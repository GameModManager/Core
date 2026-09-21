#include "ui/widgets/right_panel.h"
#include "engine/game/registry/game_capabilities.h"
#include "ui/panels/tab_panels.h"
#include "ui/widgets/column_toggle_header.h"
#include "ui/widgets/exec_controls_bar.h"
#include "ui/widgets/right_filter_bar.h"

#include <QHeaderView>
#include <QLabel>
#include <QTabWidget>
#include <QTableWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace ui {

static void setup_toggle_header(QTableWidget* table, const QStringList& labels,
                                const QStringList& tooltips = {}) {
  if (!table)
    return;
  auto* header = new ColumnToggleHeaderView(Qt::Horizontal, table);
  header->set_column_labels(labels);
  header->set_section_tooltips(tooltips);
  table->setHorizontalHeader(header);
  header->setStretchLastSection(true);
  header->setSectionsMovable(true);
}

namespace {

  // Hide a tree item unless it or any descendant matches the filter text.
  bool apply_tree_filter(QTreeWidgetItem* item, const QString& text) {
    bool self_match = text.isEmpty();
    if (!self_match) {
      for (int col = 0; col < item->columnCount(); ++col) {
        if (item->text(col).toLower().contains(text)) {
          self_match = true;
          break;
        }
      }
    }
    bool child_match = false;
    for (int i = 0; i < item->childCount(); ++i) {
      if (apply_tree_filter(item->child(i), text))
        child_match = true;
    }
    item->setHidden(!(self_match || child_match));
    return self_match || child_match;
  }

  // Capabilities with lazily built content (Workspace-j6ty). Single source
  // of truth shared by ensure_tab (placeholder gating) and build_tab (the
  // factory returns nullptr for anything else).
  bool is_lazy_tab_capability(const std::string& capability) {
    return capability == "plugins" || capability == "conflicts" ||
           capability == "archives" || capability == "saves" ||
           capability == "downloads";
  }

}  // anonymous namespace

RightPanel::RightPanel(QWidget* parent) : QWidget(parent) {
  // QSS anchor for right-panel-specific rules (e.g. #rightPanel QTableView).
  setObjectName("rightPanel");

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  exec_controls_ = new ExecControlsBar(this);
  layout->addWidget(exec_controls_);

  tab_widget_ = new QTabWidget(this);
  tab_widget_->setTabPosition(QTabWidget::North);

  // Data tab is always present - create it once
  data_tab_ = new DataTab(tab_widget_);
  tab_widget_->addTab(data_tab_, "Data");

  layout->addWidget(tab_widget_, 1);

  // Small gap so the tab pane's bottom border is visible
  layout->addSpacing(1);

  // Filter bar below tabs - persists across tab switches
  filter_bar_ = new RightFilterBar(this);
  layout->addWidget(filter_bar_);

  // Re-filter when the user switches tabs; persist the selection per
  // instance (Issue #21). Programmatic switches (set_game, restore_tab,
  // show_downloads_tab) set suppress_tab_save_ so only genuine user
  // selections reach the save handler. The first selection of a lazily
  // created tab (Workspace-j6ty) builds its real content before the filter
  // runs; intermediate swap states are skipped via materializing_.
  connect(tab_widget_, &QTabWidget::currentChanged, this, [this](int index) {
    if (index < 0 || materializing_)
      return;
    QWidget* w = tab_widget_->widget(index);
    for (const auto& [cap, placeholder] : placeholders_) {
      if (placeholder == w) {
        materialize(cap);
        break;
      }
    }
    apply_filter();
    update_sort_visibility();
    if (!suppress_tab_save_) {
      emit tab_changed(QString::fromStdString(current_tab_capability()));
    }
  });

  // Re-filter as the user types
  connect(filter_bar_, &RightFilterBar::filter_changed, this, [this]() {
    apply_filter();
  });

  // Forward the LOOT sort shortcut up to MainWindow
  connect(filter_bar_, &RightFilterBar::sort_requested, this,
          &RightPanel::sort_requested);

  // The Sort button only makes sense on the Plugins tab
  update_sort_visibility();
}

void RightPanel::update_sort_visibility() {
  const bool on_plugins_tab = tab_widget_->currentWidget() == plugins_tab();
  filter_bar_->set_sort_visible(on_plugins_tab);
}

QTableWidget* RightPanel::current_table() const {
  auto* w = tab_widget_->currentWidget();
  if (!w)
    return nullptr;

  // Each tab type exposes table() - try common patterns
  if (auto* t = w->findChild<QTableWidget*>())
    return t;
  return nullptr;
}

void RightPanel::apply_filter() {
  const QString text = filter_bar_->filter_text().trimmed().toLower();

  auto* table = current_table();
  if (table) {
    filter_bar_->apply_to(table);
    // The Plugins-tab counter is MO2-style (enabled + filter-visible), so
    // it must track the filter as it is typed.
    if (auto* pt = plugins_tab())
      pt->refresh_counters();
    // DownloadsTab: re-apply the "hide installed" filter on top of the
    // text filter. set_filter_text feeds it the current text so the
    // re-apply hides rows that fail either filter instead of unhiding the
    // text-filtered ones.
    if (auto* dt = qobject_cast<DownloadsTab*>(tab_widget_->currentWidget())) {
      dt->set_filter_text(text);
      dt->reapply_installed_filter();
    }
    return;
  }

  // ConflictsTab / DataTab use QTreeWidget - filter recursively so a branch
  // stays visible when any descendant matches.
  auto* w = tab_widget_->currentWidget();
  if (!w)
    return;
  auto* tree = w->findChild<QTreeWidget*>();
  if (tree) {
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
      apply_tree_filter(tree->topLevelItem(i), text);
  }
}

void RightPanel::clear_tabs() {
  // Remove all tabs except Data (index 0)
  while (tab_widget_->count() > 1) {
    tab_widget_->removeTab(1);
  }
  for (auto& [key, widget] : tabs_) {
    delete widget;
  }
  tabs_.clear();
  for (auto& [key, widget] : placeholders_) {
    delete widget;
  }
  placeholders_.clear();
  tab_labels_.clear();
}

void RightPanel::ensure_tab(const std::string& capability, const QString& label) {
  if (tabs_.count(capability) || placeholders_.count(capability))
    return;

  // Workspace-j6ty: show a placeholder immediately; the real tab is built
  // on first selection (currentChanged) or an ensure_* call. Unknown
  // capabilities get no tab at all (same as before).
  if (!is_lazy_tab_capability(capability))
    return;

  auto* placeholder = new QLabel(tr("Loading..."), tab_widget_);
  placeholder->setAlignment(Qt::AlignCenter);
  placeholders_[capability] = placeholder;
  tab_labels_[capability]   = label;
  tab_widget_->addTab(placeholder, label);
}

QWidget* RightPanel::build_tab(const std::string& capability) {
  QWidget* tab = nullptr;

  if (capability == "plugins") {
    auto* t = new PluginsTab(tab_widget_);
    // Column header tooltips mirror MO2's PluginList::getColumnToolTip.
    setup_toggle_header(
        t->table(),
        {tr("Plugin Name"), tr("Flags"), tr("Priority"), tr("Mod Index"), tr("Locked")},
        {tr("Name of the plugin"),
         tr("Emblems to highlight things that might require attention."),
         tr("Load priority of plugins. The higher, the more "
            "\"important\" it is and thus overwrites data from "
            "plugins with lower priority."),
         tr("Determines the formids of objects originating from this mod."),
         tr("Whether this plugin's load order position is pinned.")});
    // Mod Index keeps stretching (it used to be the last section); the
    // Locked column stays a fixed narrow slot.
    auto* hdr = t->table()->horizontalHeader();
    hdr->setStretchLastSection(false);
    hdr->setSectionResizeMode(3, QHeaderView::Stretch);
    hdr->resizeSection(4, 80);
    tab = t;
  } else if (capability == "conflicts") {
    auto* t = new ConflictsTab(tab_widget_);
    tab     = t;
  } else if (capability == "archives") {
    // Flat tree, no header.
    auto* t = new ArchivesTab(tab_widget_);
    tab     = t;
  } else if (capability == "saves") {
    auto* t = new SavesTab(tab_widget_);
    setup_toggle_header(t->table(), {tr("Name"), tr("File"), tr("Missing")});
    // Name stretches; File/Missing keep user-set width on resize.
    auto* hdr = t->table()->horizontalHeader();
    hdr->setStretchLastSection(false);
    hdr->setSectionResizeMode(0, QHeaderView::Stretch);
    hdr->setSectionResizeMode(1, QHeaderView::Interactive);
    hdr->setSectionResizeMode(2, QHeaderView::Interactive);
    hdr->resizeSection(1, 220);
    hdr->resizeSection(2, 60);
    tab = t;
  } else if (capability == "downloads") {
    auto* t = new DownloadsTab(tab_widget_);
    setup_toggle_header(t->table(),
                        {tr("Name"), tr("Source"), tr("Status"), tr("Size")});
    // Name stretches; Source/Status/Size keep user-set width on resize
    auto* hdr = t->table()->horizontalHeader();
    hdr->setStretchLastSection(false);
    hdr->setSectionResizeMode(0, QHeaderView::Stretch);
    hdr->setSectionResizeMode(1, QHeaderView::Interactive);
    hdr->setSectionResizeMode(2, QHeaderView::Interactive);
    hdr->setSectionResizeMode(3, QHeaderView::Interactive);
    hdr->resizeSection(1, 80);
    hdr->resizeSection(2, 100);
    hdr->resizeSection(3, 80);
    tab = t;
  }

  if (tab) {
    return tab;
  }
  return nullptr;
}

QWidget* RightPanel::materialize(const std::string& capability) {
  auto built = tabs_.find(capability);
  if (built != tabs_.end())
    return built->second;  // already real - emit nothing

  auto ph = placeholders_.find(capability);
  if (ph == placeholders_.end())
    return nullptr;  // capability not in the tab bar

  QWidget* placeholder = ph->second;
  const int index      = tab_widget_->indexOf(placeholder);
  QWidget* real        = build_tab(capability);
  if (!real)
    return nullptr;

  const QString label = tab_labels_[capability];
  // The remove/insert fires currentChanged for intermediate states; the
  // handler skips those via materializing_ and the setCurrentIndex below
  // restores the user's selection before any filtering runs.
  materializing_ = true;
  if (index >= 0) {
    tab_widget_->removeTab(index);  // no ownership change - delete by hand
    delete placeholder;
    tab_widget_->insertTab(index, real, label);
    tab_widget_->setCurrentIndex(index);
  } else {
    delete placeholder;
    tab_widget_->addTab(real, label);
  }
  materializing_ = false;

  placeholders_.erase(ph);
  tabs_[capability] = real;
  emit tab_materialized(QString::fromStdString(capability));
  return real;
}

void RightPanel::set_game(const std::string& game_id) {
  current_game_id_ = game_id;
  // Rebuilding the tab bar fires currentChanged for every removed/added
  // tab; none of those are user selections, so suppress tab_changed.
  suppress_tab_save_ = true;
  clear_tabs();

  if (capabilities_) {
    auto caps = capabilities_->sorted_capabilities_for(game_id);

    // Remove Data from index 0 - re-add at correct sorted position
    tab_widget_->removeTab(0);

    for (const auto& info : caps) {
      if (info.capability == "data") {
        tab_widget_->addTab(data_tab_, tr("Data"));
      } else {
        ensure_tab(info.capability, QString::fromStdString(info.display_name));
      }
    }
  }

  // Always-present tabs - instance-owned, not game-capability-owned.
  // Data: universal view of the managed game dir. Already in the tab bar
  // for known games (added by the loop above); unknown/game-less games
  // have an empty capability list, so ensure it is present.
  if (tab_widget_->indexOf(data_tab_) < 0) {
    tab_widget_->addTab(data_tab_, tr("Data"));
  }
  // Downloads: downloads_dir_path() belongs to the instance, not to any
  // game's capabilities - always visible regardless of game_id.
  ensure_tab("downloads", tr("Downloads"));

  suppress_tab_save_ = false;
}

void RightPanel::restore_tab(const std::string& capability) {
  if (capability.empty())
    return;  // default = first tab

  // A restored tab is a tab the user wants to see: build it now instead of
  // waiting for the first click.
  if (capability != "data")
    materialize(capability);

  QWidget* target = nullptr;
  if (capability == "data") {
    target = data_tab_;
  } else {
    auto it = tabs_.find(capability);
    if (it != tabs_.end())
      target = it->second;
  }
  // Unknown/unsupported capability: keep the first tab (the default).
  if (!target)
    return;

  int index = tab_widget_->indexOf(target);
  if (index < 0)
    return;

  suppress_tab_save_ = true;
  tab_widget_->setCurrentIndex(index);
  suppress_tab_save_ = false;
}

std::string RightPanel::current_tab_capability() const {
  auto* w = tab_widget_->currentWidget();
  if (!w)
    return {};
  if (w == data_tab_)
    return "data";
  for (const auto& [cap, widget] : tabs_) {
    if (widget == w)
      return cap;
  }
  // Still a placeholder (e.g. queried mid-rebuild): the capability is known
  // even though the content is not built yet.
  for (const auto& [cap, widget] : placeholders_) {
    if (widget == w)
      return cap;
  }
  return {};
}

DownloadsTab* RightPanel::downloads_tab() const {
  auto it = tabs_.find("downloads");
  if (it != tabs_.end())
    return qobject_cast<DownloadsTab*>(it->second);
  return nullptr;
}

SavesTab* RightPanel::saves_tab() const {
  auto it = tabs_.find("saves");
  if (it != tabs_.end())
    return qobject_cast<SavesTab*>(it->second);
  return nullptr;
}

void RightPanel::show_downloads_tab() {
  // A download arrived: the user should see it, so build the tab now.
  auto* dt = ensure_downloads_tab();
  if (!dt)
    return;
  int index = tab_widget_->indexOf(dt);
  if (index < 0)
    return;
  // Programmatic switch (a download arrived) - not a user selection, so
  // don't persist it as the instance's last tab.
  suppress_tab_save_ = true;
  tab_widget_->setCurrentIndex(index);
  suppress_tab_save_ = false;
}

ConflictsTab* RightPanel::conflicts_tab() const {
  auto it = tabs_.find("conflicts");
  if (it != tabs_.end())
    return qobject_cast<ConflictsTab*>(it->second);
  return nullptr;
}

DataTab* RightPanel::data_tab() const {
  return data_tab_;
}

PluginsTab* RightPanel::plugins_tab() const {
  auto it = tabs_.find("plugins");
  if (it != tabs_.end())
    return qobject_cast<PluginsTab*>(it->second);
  return nullptr;
}

DownloadsTab* RightPanel::ensure_downloads_tab() {
  if (auto* dt = downloads_tab())
    return dt;
  return qobject_cast<DownloadsTab*>(materialize("downloads"));
}

SavesTab* RightPanel::ensure_saves_tab() {
  if (auto* st = saves_tab())
    return st;
  return qobject_cast<SavesTab*>(materialize("saves"));
}

ConflictsTab* RightPanel::ensure_conflicts_tab() {
  if (auto* ct = conflicts_tab())
    return ct;
  return qobject_cast<ConflictsTab*>(materialize("conflicts"));
}

PluginsTab* RightPanel::ensure_plugins_tab() {
  if (auto* pt = plugins_tab())
    return pt;
  return qobject_cast<PluginsTab*>(materialize("plugins"));
}

}  // namespace ui
