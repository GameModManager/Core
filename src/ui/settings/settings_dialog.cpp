#include "ui/settings/settings_dialog.h"

#include <QVBoxLayout>

#include "ui/settings/settings_content_widget.h"

SettingsDialog::SettingsDialog(engine::StyleManager* style_manager,
                               const QString& native_style_name,
                               const std::filesystem::path& instance_root,
                               engine::PluginLoader* plugin_loader,
                               QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Settings"));
    setMinimumSize(ui::kSettingsMinWidth, ui::kSettingsMinHeight);
    resize(820, 720);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    content_ = new ui::SettingsContentWidget(style_manager, native_style_name,
                                             instance_root, plugin_loader, this);
    outer->addWidget(content_);
}
