#include "ui/settings/settings_dialog.h"

#include <QVBoxLayout>

#include "ui/controllers/settings_controller.h"
#include "ui/settings/settings_content_widget.h"

SettingsDialog::SettingsDialog(engine::StyleManager* style_manager,
                               const QString& native_style_name,
                               const std::filesystem::path& instance_root,
                               engine::PluginLoader* plugin_loader,
                               ui::SettingsController* settings_controller,
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

    // Wire the Diagnostics tab's "Show DEBUG Panel" button to the controller
    // that owns the DebugWindow. The signal is declared on the content widget
    // so the host (this dialog or the tab-mode router) is responsible for
    // bridging it to the controller.
    if (settings_controller) {
        connect(content_, &ui::SettingsContentWidget::open_debug_panel_requested,
                settings_controller,
                &ui::SettingsController::show_debug_window);
    }
}
