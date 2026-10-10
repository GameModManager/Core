#pragma once

#include "engine/game/esp/plugin_info.h"
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
  /// Name filter over the table, driven by the shared RightFilterBar text.
  /// See PluginView::apply_filter.
  void apply_filter(const QString &text);
  void set_contained_plugins(const QVector<QString> &contained);
  void set_master_plugins(const QVector<QString> &masters);
  [[nodiscard]] QStringList selected_plugin_names() const;

  // Column role constants (forwarded from PluginView for callers that
  // reference them via PluginsTab::kPluginFlagsRole).
  static constexpr int kPluginFlagsRole        = PluginView::kPluginFlagsRole;
  static constexpr int kPluginFlagTooltipsRole = PluginView::kPluginFlagTooltipsRole;

signals:
  void toggle_requested(const std::string &name, bool enabled);
  void reorder_requested(int from_row, int to_row);
  void lock_requested(const std::string &name, bool locked);
  // Forwarded from the context menu's bulk Enable all / Disable all pair.
  void set_all_requested(bool enabled);
  void refresh_requested();
  // Forwarded from PluginView: the backup / restore pair beside the "Active:"
  // counter. Both are manual-only actions.
  void backup_requested();
  void restore_requested();
  // Forwarded from PluginView: double-clicking a row asks for the owning
  // mod's Mod Info / folder reveal. Owner id is "" only for game-Data rows,
  // which never emit.
  void mod_info_requested(const std::string &owner_mod);
  void reveal_requested(const std::string &owner_mod);
  // Forwarded from PluginView's Ctrl+Up / Ctrl+Down handler.
  void shift_requested(const std::vector<int> &rows, int offset);

protected:
  void add_context_menu_actions(QMenu &menu, int row);
  void showEvent(QShowEvent *event) override;

private:
  void on_custom_context_menu(const QPoint &pos);
  // Hand the view's per-row metadata to the extracted context menu. Called
  // from set_plugins and sync_enabled so the menu's state labels stay live.
  void refresh_context_rows();

  PluginView *view_ = nullptr;
  std::unique_ptr<engine::PluginDb::ContextMenu> context_menu_;
};

}  // namespace ui
