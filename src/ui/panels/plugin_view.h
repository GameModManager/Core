#pragma once

#include "engine/game/plugins/plugin_info.h"

#include <QPoint>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include <memory>
#include <string>
#include <vector>

class QLCDNumber;
class QPushButton;
class QTableWidget;

namespace ui {

// PluginView is the table-based view that renders the plugin list.
// Extracted from PluginsTab (issue/restructure) so the table rendering,
// counter, and highlight logic live in a focused class.  PluginsTab
// becomes a thin container that owns a PluginView + the context menu.
class PluginView : public QWidget {
  Q_OBJECT
public:
  explicit PluginView(QWidget *parent = nullptr);

  /// The underlying QTableWidget (for external delegates / selection queries).
  [[nodiscard]] QTableWidget *table() const;

  // --- Plugin data ---------------------------------------------------------

  /// Replace the plugin list contents. Row 0 = most dominant (first-loaded).
  /// Force-loaded rows (game-native, CC) are pinned and shown greyed.
  void set_plugins(const std::vector<engine::GamePlugin> &plugins);

  /// Re-sync enabled checkboxes from engine state without rebuilding rows.
  void sync_enabled(const std::vector<engine::GamePlugin> &plugins);

  /// MO2-style plugin counter (PluginListView::updatePluginCount parity).
  void refresh_counters();

  /// MO2 parity - highlight rows owned by the selected mod / master plugins.
  void set_contained_plugins(const QVector<QString> &contained);
  void set_master_plugins(const QVector<QString> &masters);

  /// Names of the plugins currently selected in the table (row order).
  [[nodiscard]] QStringList selected_plugin_names() const;

  // --- Per-row metadata (for context menu consumers) ------------------------

  [[nodiscard]] const std::vector<std::string> &names() const { return names_; }
  [[nodiscard]] const std::vector<bool> &rows_locked() const {
    return rows_locked_;
  }
  [[nodiscard]] const std::vector<bool> &rows_force_loaded() const {
    return rows_force_loaded_;
  }

  // --- Column role constants -----------------------------------------------

  // User role on the Flags column holding the row's emblems as individual
  // QIcons (QList<QIcon>).
  static constexpr int kPluginFlagsRole = Qt::UserRole + 60;
  // Parallel role: per-emblem hover text (QStringList).
  static constexpr int kPluginFlagTooltipsRole = Qt::UserRole + 61;

signals:
  void toggle_requested(const std::string &name, bool enabled);
  void reorder_requested(int from_row, int to_row);
  /// Refresh button pressed: re-scan plugins on disk and repopulate.
  void refresh_requested();

protected:
  void showEvent(QShowEvent *event) override;

private:
  void apply_highlights();
  void relayout_flag_rows();

  // MO2 plugin classification for the counter.
  enum class PluginType { Regular, Master, Light, Medium };

  class PluginTable;
  PluginTable *table_ = nullptr;
  QPushButton *refresh_button_ = nullptr;
  QLCDNumber *counter_display_ = nullptr;
  std::vector<std::string> names_;
  std::vector<bool> rows_locked_;
  std::vector<bool> rows_force_loaded_;
  std::vector<PluginType> rows_type_;
  QSet<QString> contained_names_;
  QSet<QString> master_names_;
  bool syncing_ = false;
};

} // namespace ui
