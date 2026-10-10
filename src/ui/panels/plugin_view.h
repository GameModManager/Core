#pragma once

#include "engine/game/esp/plugin_info.h"

#include <QPoint>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include <memory>
#include <string>
#include <vector>

class QKeyEvent;
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

  /// MO2's espFilterEdit (mainwindow.ui) as PluginListView wires it
  /// (pluginlistview.cpp:259-262): a name filter over the plugin table. Rows
  /// that do not match are hidden, not removed, so enable state and the
  /// load order are untouched; the counter then reports the active count of
  /// what is still visible, which is the MO2 behaviour
  /// (pluginlistview.cpp:86, :165).
  ///
  /// The text comes from the shared RightFilterBar below the tab bar, the same
  /// bar Data, Downloads, Saves and Conflicts use - there is no plugin-side
  /// input. RightPanel routes it here so the match is on the plugin NAME
  /// alone, which is what espFilterEdit does; the generic all-column pass the
  /// other tabs get would also hide a row on its Priority or Mod Index text.
  /// The last text is remembered so a Refresh (set_plugins) re-applies the
  /// filter the user is still looking at instead of silently showing
  /// everything again.
  void apply_filter(const QString &text);

  /// MO2 parity - highlight rows owned by the selected mod / master plugins.
  void set_contained_plugins(const QVector<QString> &contained);
  void set_master_plugins(const QVector<QString> &masters);

  /// Names of the plugins currently selected in the table (row order).
  [[nodiscard]] QStringList selected_plugin_names() const;

  // --- Per-row metadata (for context menu consumers) ------------------------

  [[nodiscard]] const std::vector<std::string> &names() const { return names_; }
  [[nodiscard]] const std::vector<bool> &rows_locked() const { return rows_locked_; }
  [[nodiscard]] const std::vector<bool> &rows_force_loaded() const {
    return rows_force_loaded_;
  }
  // Per-row enable state as rendered (core rows count as enabled). The
  // context menu labels its Enable/Disable action from this.
  [[nodiscard]] const std::vector<bool> &rows_enabled() const { return rows_enabled_; }

  /// Mod folder providing row `row`, "" for a game-Data (unowned) plugin or
  /// an out-of-range row. Parallel to names(), same order as the table rows.
  [[nodiscard]] std::string owner_mod_at(int row) const {
    if (row < 0 || row >= static_cast<int>(owners_.size()))
      return {};
    return owners_[static_cast<size_t>(row)];
  }

  // --- Column role constants -----------------------------------------------

  // User role on the Flags column holding the row's emblems as individual
  // QIcons (QList<QIcon>).
  static constexpr int kPluginFlagsRole = Qt::UserRole + 60;
  // Parallel role: per-emblem hover text (QStringList).
  static constexpr int kPluginFlagTooltipsRole = Qt::UserRole + 61;

  // Column order (setHorizontalHeaderLabels in the ctor). Named so the rest
  // of the UI talks about columns by name rather than a bare integer, the way
  // MO2 does (PluginList::COL_PRIORITY).
  enum Col { ColName = 0, ColFlags, ColPriority, ColModIndex, ColLocked };

signals:
  void toggle_requested(const std::string &name, bool enabled);
  void reorder_requested(int from_row, int to_row);
  /// Refresh button pressed: re-scan plugins on disk and repopulate.
  void refresh_requested();
  /// Backup / Restore pressed: write or read a timestamped copy of
  /// plugins.txt + loadorder.txt + lockedorder.txt beside the live files
  /// (MO2 mainwindow.ui:840,863). Both are MANUAL only -
  /// MO2 has no automatic trigger, and neither do we.
  void backup_requested();
  void restore_requested();
  /// Double-clicked a plugin row: ask for the Mod Info dialog of the mod
  /// owning it. Never emitted for a game-Data plugin (no owner) - the view
  /// drops those, so an unowned row is simply inert.
  void mod_info_requested(const std::string &owner_mod);
  /// Ctrl+Double-clicked a plugin row: ask to reveal (open the OS file
  /// manager at) the folder of the mod owning it.
  void reveal_requested(const std::string &owner_mod);
  /// Ctrl+Up / Ctrl+Down on the plugin table: shift every row in `rows` one
  /// place in the load order (MO2 PluginList::shiftPluginsPriority). The
  /// view has no load order of its own, so it reports the rows and the
  /// offset; the controller moves them and refreshes.
  void shift_requested(const std::vector<int> &rows, int offset);

protected:
  void showEvent(QShowEvent *event) override;

private:
  void apply_highlights();
  void relayout_flag_rows();
  // MO2 PluginListView::event keyboard routing; true = key consumed.
  bool handle_key(QKeyEvent *event);

  // MO2 plugin classification for the counter.
  enum class PluginType { Regular, Master, Light, Medium };

  class PluginTable;
  PluginTable *table_          = nullptr;
  QPushButton *refresh_button_ = nullptr;
  QPushButton *backup_button_  = nullptr;
  QPushButton *restore_button_ = nullptr;
  QLCDNumber *counter_display_ = nullptr;
  std::vector<std::string> names_;
  // Mod folder providing each row's plugin ("" = game Data), parallel to
  // names_. The double-click handlers need it: a plugin's own name says
  // nothing about where the owning mod lives.
  std::vector<std::string> owners_;
  std::vector<bool> rows_locked_;
  std::vector<bool> rows_force_loaded_;
  std::vector<bool> rows_enabled_;
  std::vector<PluginType> rows_type_;
  QSet<QString> contained_names_;
  QSet<QString> master_names_;
  // Last text handed to apply_filter, so set_plugins can re-apply it.
  QString filter_text_;
  bool syncing_ = false;
};

}  // namespace ui
