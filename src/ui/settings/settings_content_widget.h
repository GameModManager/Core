#pragma once

#include <QWidget>

#include <filesystem>

class QTabWidget;

namespace engine {
class PluginLoader;
class StyleManager;
} // namespace engine

namespace ui {

// Minimum geometry the settings panel needs to lay out its widest tab (the
// Plugins tab's two columns) at a usable size. The popup host (SettingsDialog)
// uses both as its window minimum; the Plugins tab itself uses the width to
// keep the left plugin list at half the minimum window width.
inline constexpr int kSettingsMinWidth = 723;
inline constexpr int kSettingsMinHeight = 634;

// Mode-agnostic settings panel. Extracted from SettingsDialog so the same
// content can be embedded either in a popup QDialog (SettingsDialog) or as a
// tab page inside MainTabContainer (Full UI tab mode). Mirrors MO2's settings
// dialog layout (General, Theme, Mod List, Paths, Sources, Plugins,
// Workarounds, Diagnostics) in an internal QTabWidget. Controls write their
// setting immediately on change; restart-required items show an inline hint.
class SettingsContentWidget : public QWidget {
  Q_OBJECT
public:
  SettingsContentWidget(engine::StyleManager *style_manager,
                        const QString &native_style_name,
                        const std::filesystem::path &instance_root,
                        engine::PluginLoader *plugin_loader,
                        QWidget *parent = nullptr);

  // The internal tab widget (General, Theme, ...). Exposed so embedding
  // hosts can select tabs programmatically.
  QTabWidget *tab_widget() const { return tabs_; }

signals:
  // Emitted when the "Enable full UI tab mode" checkbox is toggled, so the
  // TabModeController can react live (tab bar visibility, closing tabs).
  void full_ui_mode_toggled(bool on);
  // Emitted when the user clicks the "Show DEBUG Panel" button in the
  // Diagnostics tab. Wired by the host (SettingsDialog in popup mode, the
  // tab-mode controller in Full UI mode) to SettingsController, which owns
  // the DebugWindow.
  void open_debug_panel_requested();

private:
  QWidget *build_general_tab();
  QWidget *build_theme_tab();
  QWidget *build_modlist_tab();
  QWidget *build_paths_tab();
  QWidget *build_sources_tab();
  QWidget *build_plugins_tab();
  QWidget *build_workarounds_tab();
  QWidget *build_diagnostics_tab();

  engine::StyleManager *style_manager_;
  QString native_style_name_;
  std::filesystem::path instance_root_;
  engine::PluginLoader *plugin_loader_ = nullptr;
  QTabWidget *tabs_ = nullptr;
};

} // namespace ui
