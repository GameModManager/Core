#pragma once

#include "engine/core/instance/instance_utils.h"

#include <QDialog>

#include <filesystem>
#include <string>

namespace engine {
class Platform;
class PluginLoader;
}

namespace ui {

class InstanceOptionsWidget;

// Modal "Instance Options" dialog: thin QDialog wrapper around the
// mode-agnostic InstanceOptionsWidget. The dialog's Save/Close buttons map to
// the widget's save_requested()/cancel_requested() signals; the caller
// persists the selected runner to instance.toml after exec() returns
// (MainWindow / LaunchController), following the SettingsDialog pattern.
//
// All content (runner selector, recommended wine packages, deploy management
// with inline progress) lives in InstanceOptionsWidget, so the same panel can
// be embedded as a tab page in Full UI tab mode.
class InstanceOptionsDialog : public QDialog {
    Q_OBJECT
public:
    InstanceOptionsDialog(engine::Platform* platform,
                          engine::PluginLoader* plugin_loader,
                          const std::string& game_id,
                          const std::string& game_display_name,
                          const std::filesystem::path& game_dir,
                          uint32_t steam_appid,
                          const std::filesystem::path& instance_root,
                          const std::string& current_runner,
                          const std::string& current_deploy_strategy,
                          const engine::DeployConfig& deploy_config,
                          QWidget* parent = nullptr);

    ~InstanceOptionsDialog() override;

    // Runner selected in the panel (display name or absolute path).
    // Empty = automatic (Steam per-game override, then latest).
    [[nodiscard]] std::string selected_runner() const;

    // The embedded content widget, so the host can wire host-owned
    // capabilities (e.g. the deferred disable queue flush) onto it.
    [[nodiscard]] InstanceOptionsWidget* content() const { return content_; }

private:
    InstanceOptionsWidget* content_ = nullptr;
};

}  // namespace ui
