#include "ui/instance_options/instance_options_panel.h"

#include "ui/instance_options/instance_options_widget.h"

#include <QVBoxLayout>

namespace ui {

InstanceOptionsDialog::InstanceOptionsDialog(
    engine::Platform* platform, engine::PluginLoader* plugin_loader,
    const std::string& game_id, const std::string& game_display_name,
    const std::filesystem::path& game_dir, uint32_t steam_appid,
    const std::filesystem::path& instance_root,
    const std::string& current_runner,
    const std::string& current_deploy_strategy,
    const engine::DeployConfig& deploy_config, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(
        tr("Instance Options - %1").arg(QString::fromStdString(game_display_name)));

    auto* layout = new QVBoxLayout(this);
    content_ = new InstanceOptionsWidget(platform, plugin_loader, game_id,
                                         game_display_name, game_dir,
                                         steam_appid, instance_root,
                                         current_runner, current_deploy_strategy,
                                         deploy_config, this);
    layout->addWidget(content_);

    // The content widget's Save/Close buttons drive the dialog result; the
    // caller persists the selected runner after exec() returns Accepted.
    connect(content_, &InstanceOptionsWidget::save_requested, this,
            &QDialog::accept);
    connect(content_, &InstanceOptionsWidget::cancel_requested, this,
            &QDialog::reject);
}

InstanceOptionsDialog::~InstanceOptionsDialog() = default;

std::string InstanceOptionsDialog::selected_runner() const {
    return content_ ? content_->selected_runner() : std::string();
}

}  // namespace ui
