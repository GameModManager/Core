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
  layout->addWidget(view_);

  // Forward PluginView signals.
  connect(view_, &PluginView::toggle_requested, this,
          &PluginsTab::toggle_requested);
  connect(view_, &PluginView::reorder_requested, this,
          &PluginsTab::reorder_requested);
  connect(view_, &PluginView::refresh_requested, this,
          &PluginsTab::refresh_requested);

  // Extracted context menu (lock/unlock actions).
  context_menu_ = std::make_unique<engine::PluginDb::ContextMenu>(this);
  connect(context_menu_.get(), &engine::PluginDb::ContextMenu::lock_requested,
          this, &PluginsTab::lock_requested);

  // Right-click context menu on the table.
  connect(view_->table(), &QTableWidget::customContextMenuRequested, this,
          &PluginsTab::on_custom_context_menu);
}

// --- Forwarded PluginView API -----------------------------------------------

void PluginsTab::set_plugins(const std::vector<engine::GamePlugin> &plugins) {
  view_->set_plugins(plugins);
  // Populate the extracted context menu with per-row metadata.
  std::vector<engine::PluginDb::RowInfo> row_infos;
  const auto &names = view_->names();
  const auto &locked = view_->rows_locked();
  const auto &force = view_->rows_force_loaded();
  row_infos.reserve(names.size());
  for (size_t i = 0; i < names.size(); ++i) {
    engine::PluginDb::RowInfo ri;
    ri.name = names[i];
    ri.locked = i < locked.size() && locked[i];
    ri.force_loaded = i < force.size() && force[i];
    row_infos.push_back(std::move(ri));
  }
  context_menu_->set_rows(std::move(row_infos));
}

void PluginsTab::sync_enabled(const std::vector<engine::GamePlugin> &plugins) {
  view_->sync_enabled(plugins);
}

void PluginsTab::refresh_counters() { view_->refresh_counters(); }

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
  auto *t = view_->table();
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

} // namespace ui
