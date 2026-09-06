#pragma once

// Right-click on a save > "Information..." opens this dialog. The content
// is a fixed 2-column layout: the left column is a thumbnail above a basic-
// info block, the right column is the full list of plugins the save knows
// about with a per-row have/missing verdict cross-checked against the
// active profile load order (PluginDb::Database). MO2
// GamebryoSaveGameInfoWidget spirit, but windowed instead of hover.
//
// All inputs are value copies taken on the caller's thread (the SaveGame
// from the Saves scan + a snapshot of the current load order). The dialog
// never re-scans; it just renders what it was handed. Single-shot, modal,
// geometry persisted via Settings::saveinfo_window_geometry.

#include "engine/game/plugins/plugin_info.h"
#include "engine/game/saves/save_game.h"
#include "engine/game/saves/save_missing_assets.h"

#include <QDialog>

#include <vector>

class QDialogButtonBox;
class QLabel;
class QListWidget;

namespace ui
{

class SaveInfoDialog : public QDialog
{
  Q_OBJECT
public:
  // Plugins snapshot is the active load order (PluginDb::Database::plugins()).
  // Missing is the resolver's result for this save against the SAME snapshot
  // — passed in so the tooltip columns match what the Saves tab reports
  // (provider mods etc.) without re-running the resolver.
  explicit SaveInfoDialog(const engine::SaveGame& save,
                          const std::vector<engine::GamePlugin>& plugins_snapshot,
                          const std::vector<engine::SaveMissingAsset>& missing,
                          QWidget* parent = nullptr);

  int exec() override;  // saves/restores geometry

private:
  void build_thumbnail(QLabel* target) const;
  void build_basic_info(QWidget* container) const;
  void build_plugin_list(QListWidget* target) const;

  // Header label above the plugin list; owned by the dialog. Set up in the
  // ctor so build_plugin_list only has to add rows.
  QLabel* plugin_header_ = nullptr;

  // Snapshot fields owned by copy (the caller's SaveGame may go away while
  // the dialog is open if the user re-scans).
  engine::SaveGame save_;
  std::vector<engine::GamePlugin> plugins_;
  std::vector<engine::SaveMissingAsset> missing_;
};

}  // namespace ui
