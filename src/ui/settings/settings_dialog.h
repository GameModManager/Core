#pragma once

#include <QDialog>

#include <filesystem>
#include <string>

namespace engine {
class PluginLoader;
class StyleManager;
} // namespace engine

namespace ui {
class SettingsContentWidget;
}

class QCloseEvent;

// Tabbed settings dialog. Thin QDialog wrapper around the mode-agnostic
// SettingsContentWidget: the panel plus a Close button. All controls write
// their setting immediately on change; restart-required items show an inline
// hint. In Full UI tab mode the same panel is embedded directly in
// MainTabContainer, so this dialog is only used for popup mode.
class SettingsDialog : public QDialog {
  Q_OBJECT
public:
  SettingsDialog(engine::StyleManager *style_manager,
                 const QString &native_style_name,
                 const std::filesystem::path &instance_root,
                 engine::PluginLoader *plugin_loader,
                 QWidget *parent = nullptr);

signals:
  // Emitted when the "Enable full UI tab mode" checkbox is toggled, so the
  // TabModeController can react live (tab bar visibility, closing tabs).
  void full_ui_mode_toggled(bool on);
  // Emitted when the dialog is closed (Close button, window X, ...). Lets
  // an embedding host (Full UI tab mode) drop the tab that holds it.
  void closed();

protected:
  void closeEvent(QCloseEvent *event) override;

private:
  ui::SettingsContentWidget *content_ = nullptr;
};
