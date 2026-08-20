#pragma once

#include <QDialog>

#include <filesystem>

namespace engine {
class PluginLoader;
class StyleManager;
}

class QWidget;

namespace ui {
class SettingsContentWidget;
}

// Thin QDialog wrapper around SettingsContentWidget.  Used when Full UI mode
// is OFF (standalone window); Full UI tab mode embeds the content widget
// directly in MainTabContainer.
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    SettingsDialog(engine::StyleManager* style_manager,
                   const QString& native_style_name,
                   const std::filesystem::path& instance_root,
                   engine::PluginLoader* plugin_loader,
                   QWidget* parent = nullptr);

private:
    ui::SettingsContentWidget* content_ = nullptr;
};
