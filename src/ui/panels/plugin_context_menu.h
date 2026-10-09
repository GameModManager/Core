#pragma once

#include <QObject>
#include <QString>

#include <string>
#include <vector>

class QMenu;

namespace engine::PluginDb {

// Per-row metadata needed by the context menu (extracted from PluginsTab).
struct RowInfo {
  std::string name;
  bool locked       = false;
  bool force_loaded = false;
  bool enabled      = false;
  // Mod folder providing this plugin, "" for a game-Data (vanilla or CC)
  // row. MO2 gates the Open Origin pair on its origin() resolving to a mod
  // (pluginlistcontextmenu.cpp:91-107); an unowned row is the same condition,
  // because a game file belongs to no mod.
  std::string owner_mod;
};

// Context menu for the plugin table (MO2 PluginListContextMenu parity).
// Receives row metadata and emits lock/unlock requests.
class ContextMenu : public QObject {
  Q_OBJECT
public:
  explicit ContextMenu(QObject *parent = nullptr);

  // Update the per-row metadata (called when the plugin list is rebuilt).
  void set_rows(const std::vector<RowInfo> &rows);

  // Fill `menu` with actions for the given row index.
  // Split out so tests can drive it without exec()-ing a modal menu.
  void add_actions(QMenu &menu, int row);

signals:
  void lock_requested(const std::string &name, bool locked);
  // "Enable selected" / "Disable selected": flip one plugin's state.
  void toggle_requested(const std::string &name, bool enabled);
  // "Send to... Top / Bottom / Priority...": move one plugin to a new row.
  // Same signal the table's drag reorder uses, so one handler serves both.
  void reorder_requested(int from_row, int to_row);
  // "Enable all" / "Disable all" (MO2 pluginlistcontextmenu.cpp:37-49).
  // Emitted regardless of the right-clicked row, so it is on the menu of
  // every row and of the table itself.
  void set_all_requested(bool enabled);
  // "Open Origin in Explorer": reveal the mod folder providing this row's
  // plugin (MO2 PluginListContextMenu::openOriginExplorer).
  void open_origin_explorer_requested(const std::string &owner);
  // "Open Origin Info...": open that mod's Mod Info dialog
  // (MO2 PluginListContextMenu::openOriginInformation).
  void open_origin_info_requested(const std::string &owner);

private:
  std::vector<RowInfo> rows_;
};

}  // namespace engine::PluginDb
