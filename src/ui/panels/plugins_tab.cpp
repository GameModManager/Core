#include "ui/panels/plugins_tab.h"

#include <QMenu>
#include <QPoint>
#include <QShowEvent>
#include <QTableWidget>
#include <QVBoxLayout>

#include <string>
#include <vector>

namespace ui {

QTableWidget *PluginsTab::table() const {
  return view_ ? view_->table() : nullptr;
}

PluginsTab::PluginsTab(QWidget *parent) : QWidget(parent) {
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);

  view_ = new PluginView(this);
  view_->setWhatsThis(
      tr("List of the instance's plugin files, in the order they are loaded. Drag "
         "rows to reorder them yourself, or use \"Sort\" below to have LOOT do it."));
  layout->addWidget(view_);

  // Forward PluginView signals.
  connect(view_, &PluginView::toggle_requested, this, &PluginsTab::toggle_requested);
  connect(view_, &PluginView::reorder_requested, this, &PluginsTab::reorder_requested);
  connect(view_, &PluginView::refresh_requested, this, &PluginsTab::refresh_requested);
  connect(view_, &PluginView::backup_requested, this, &PluginsTab::backup_requested);
  connect(view_, &PluginView::restore_requested, this, &PluginsTab::restore_requested);
  connect(view_, &PluginView::mod_info_requested, this, &PluginsTab::mod_info_requested);
  connect(view_, &PluginView::reveal_requested, this, &PluginsTab::reveal_requested);

  // Extracted context menu (enable/disable, send-to, lock/unlock actions).
  context_menu_ = std::make_unique<engine::PluginDb::ContextMenu>(this);
  connect(context_menu_.get(), &engine::PluginDb::ContextMenu::lock_requested, this,
          &PluginsTab::lock_requested);
  connect(context_menu_.get(), &engine::PluginDb::ContextMenu::toggle_requested, this,
          &PluginsTab::toggle_requested);
  connect(context_menu_.get(), &engine::PluginDb::ContextMenu::reorder_requested, this,
          &PluginsTab::reorder_requested);
  connect(context_menu_.get(), &engine::PluginDb::ContextMenu::set_all_requested, this,
          &PluginsTab::set_all_requested);
  // Ctrl+Up / Ctrl+Down keyboard shift, forwarded from the view.
  connect(view_, &PluginView::shift_requested, this, &PluginsTab::shift_requested);
  // "Open Origin in Explorer" / "Open Origin Info..." reuse the pair the
  // view already emits for a double-click on the same row, so one controller
  // handler serves both routes to a mod.
  connect(context_menu_.get(),
          &engine::PluginDb::ContextMenu::open_origin_explorer_requested, this,
          &PluginsTab::reveal_requested);
  connect(context_menu_.get(),
          &engine::PluginDb::ContextMenu::open_origin_info_requested, this,
          &PluginsTab::mod_info_requested);

  // Right-click context menu on the table. The policy MUST be
  // Qt::CustomContextMenu or customContextMenuRequested never fires (regression
  // Workspace-a3t7: the PluginView extraction kept the connect but dropped this
  // line, killing the menu).
  view_->table()->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(view_->table(), &QTableWidget::customContextMenuRequested, this,
          &PluginsTab::on_custom_context_menu);
}

// --- Forwarded PluginView API -----------------------------------------------

void PluginsTab::set_plugins(const std::vector<engine::GamePlugin> &plugins) {
  view_->set_plugins(plugins);
  refresh_context_rows();
}

void PluginsTab::sync_enabled(const std::vector<engine::GamePlugin> &plugins) {
  view_->sync_enabled(plugins);
  // A toggle changes the row's enable state without a rebuild, and the menu
  // labels its Enable/Disable action from it - so the cached rows go stale
  // otherwise and the next right-click shows the wrong verb.
  refresh_context_rows();
}

// Hand the view's per-row metadata to the extracted context menu.
void PluginsTab::refresh_context_rows() {
  std::vector<engine::PluginDb::RowInfo> row_infos;
  const auto &names   = view_->names();
  const auto &locked  = view_->rows_locked();
  const auto &force   = view_->rows_force_loaded();
  const auto &enabled = view_->rows_enabled();
  row_infos.reserve(names.size());
  for (size_t i = 0; i < names.size(); ++i) {
    engine::PluginDb::RowInfo ri;
    ri.name         = names[i];
    ri.locked       = i < locked.size() && locked[i];
    ri.force_loaded = i < force.size() && force[i];
    ri.enabled      = i < enabled.size() && enabled[i];
    ri.owner_mod    = view_->owner_mod_at(static_cast<int>(i));
    row_infos.push_back(std::move(ri));
  }
  context_menu_->set_rows(std::move(row_infos));
}

void PluginsTab::refresh_counters() {
  view_->refresh_counters();
}

void PluginsTab::set_contained_plugins(const QVector<QString> &contained) {
  view_->set_contained_plugins(contained);
}

void PluginsTab::set_master_plugins(const QVector<QString> &masters) {
  view_->set_master_plugins(masters);
}

QStringList PluginsTab::selected_plugin_names() const {
  return view_->selected_plugin_names();
}

// --- Context menu -----------------------------------------------------------

void PluginsTab::add_context_menu_actions(QMenu &menu, int row) {
  context_menu_->add_actions(menu, row);
}

void PluginsTab::on_custom_context_menu(const QPoint &pos) {
  auto *t       = view_->table();
  const int row = t->rowAt(pos.y());
  QMenu menu(this);
  add_context_menu_actions(menu, row);
  if (menu.actions().isEmpty())
    return;
  menu.exec(t->viewport()->mapToGlobal(pos));
}

void PluginsTab::showEvent(QShowEvent *event) {
  QWidget::showEvent(event);
  view_->refresh_counters();
}

}  // namespace ui
