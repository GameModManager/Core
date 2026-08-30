#pragma once

#include "engine/game/plugins/plugin_info.h"
#include "ui/panels/plugin_context_menu.h"
#include "ui/panels/plugin_view.h"

#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include <memory>
#include <string>
#include <vector>

class QMenu;

namespace ui {

// PluginsTab is now a thin container owning a PluginView (the table) and a
// PluginContextMenu (lock/unlock actions).  All table rendering, counter,
// and highlight logic lives in PluginView.
class PluginsTab : public QWidget {
  Q_OBJECT
public:
  explicit PluginsTab(QWidget *parent = nullptr);

  /// The underlying QTableWidget (forwarded from PluginView).
  [[nodiscard]] QTableWidget *table() const;

  /// Access the contained PluginView for direct use.
  [[nodiscard]] PluginView *plugin_view() const { return view_; }

  // --- Forwarded PluginView API -------------------------------------------

  void set_plugins(const std::vector<engine::GamePlugin> &plugins);
  void sync_enabled(const std::vector<engine::GamePlugin> &plugins);
  void refresh_counters();
  void set_contained_plugins(const QVector<QString> &contained);
  void set_master_plugins(const QVector<QString> &masters);
  [[nodiscard]] QStringList selected_plugin_names() const;

  // Column role constants (forwarded from PluginView for callers that
  // reference them via PluginsTab::kPluginFlagsRole).
  static constexpr int kPluginFlagsRole = PluginView::kPluginFlagsRole;
  static constexpr int kPluginFlagTooltipsRole =
      PluginView::kPluginFlagTooltipsRole;

signals:
  void toggle_requested(const std::string &name, bool enabled);
  void reorder_requested(int from_row, int to_row);
  void lock_requested(const std::string &name, bool locked);
  void refresh_requested();

protected:
  void add_context_menu_actions(QMenu &menu, int row);
  void showEvent(QShowEvent *event) override;

private:
  void on_custom_context_menu(const QPoint &pos);

  PluginView *view_ = nullptr;
  std::unique_ptr<engine::PluginDb::ContextMenu> context_menu_;
};

} // namespace ui
