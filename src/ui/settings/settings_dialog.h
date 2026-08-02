#pragma once

#include <QDialog>

#include <filesystem>
#include <string>

namespace engine {
class PluginLoader;
class StyleManager;
}

class QWidget;

// Tabbed settings panel. Mirrors MO2's settings dialog layout (General,
// Theme, Mod List, Paths, Sources, Plugins, Workarounds, Diagnostics).
// Controls write their setting immediately on change; restart-required
// items show an inline hint.
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    SettingsDialog(engine::StyleManager* style_manager,
                   const QString& native_style_name,
                   const std::filesystem::path& instance_root,
                   engine::PluginLoader* plugin_loader,
                   QWidget* parent = nullptr);

private:
    QWidget* build_general_tab();
    QWidget* build_theme_tab();
    QWidget* build_modlist_tab();
    QWidget* build_paths_tab();
    QWidget* build_sources_tab();
    QWidget* build_plugins_tab();
    QWidget* build_workarounds_tab();
    QWidget* build_diagnostics_tab();

    engine::StyleManager* style_manager_;
    QString native_style_name_;
    std::filesystem::path instance_root_;
    engine::PluginLoader* plugin_loader_ = nullptr;
};
