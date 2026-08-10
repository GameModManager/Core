#pragma once

#include "engine/instance/instance_utils.h"

#include <QDialog>

#include <filesystem>
#include <string>

class QComboBox;
class QLabel;
class QProgressDialog;
class QPushButton;
class QThread;
class QVBoxLayout;

namespace engine {
class PlatformInterface;
class PluginLoader;
}

namespace ui {

// Modal "Proton options" panel: per-instance Proton runner selector plus the
// game's recommended wine packages (wine.json shipped with the game plugin).
// The runner is persisted to instance.toml by the caller (MainWindow) after
// exec(), following the SettingsDialog pattern.
//
// It also hosts a "Deploy management" section: a "Deployment strategy" dropdown
// (only the strategies the program + host actually support are listed) whose
// selection is persisted to instance.toml and honored by the launch path, plus
// — for direct (symlink) deploys only — "Force re-deploy links" (tears down the
// current deploy, restoring any original game files parked in
// <game_dir>/Original_Files, then re-deploys all enabled mods) and "Remove
// deployed files" (teardown only, returning the game to its pristine unmodded
// state). The task buttons run on a background thread with a modal progress
// dialog (THREADING.md: the panel's event loop keeps pumping, so queued
// progress/result callbacks are delivered; no engine call ever runs on the main
// thread).
class ProtonPanel : public QDialog {
    Q_OBJECT
public:
    ProtonPanel(engine::PlatformInterface* platform,
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

    ~ProtonPanel() override;

    // Runner selected in the panel (display name or absolute path).
    // Empty = automatic (Steam per-game override, then latest).
    [[nodiscard]] std::string selected_runner() const;

private:
    enum class DeployTaskKind { Redeploy, Remove };

    void refresh_runners();
    void update_runner_detail();
    void load_recommended_packages();
    void install_packages(const QStringList& verbs);
    void build_deploy_management();
    void update_deploy_actions_enabled();
    void run_deploy_task(DeployTaskKind kind);
    void finish_deploy_task(DeployTaskKind kind, bool ok);
    [[nodiscard]] std::filesystem::path recommended_packages_path() const;

    engine::PlatformInterface* platform_ = nullptr;
    engine::PluginLoader* plugin_loader_ = nullptr;
    std::string game_id_;
    std::string game_display_name_;
    std::filesystem::path game_dir_;
    uint32_t steam_appid_ = 0;
    std::filesystem::path instance_root_;
    std::string current_deploy_strategy_;
    engine::DeployConfig deploy_config_;

    QComboBox* runner_combo_ = nullptr;
    QLabel* runner_detail_ = nullptr;
    QPushButton* install_all_btn_ = nullptr;
    QVBoxLayout* packages_layout_ = nullptr;
    QLabel* packages_status_ = nullptr;
    QComboBox* deploy_strategy_combo_ = nullptr;
    QPushButton* redeploy_btn_ = nullptr;
    QPushButton* remove_btn_ = nullptr;
    QProgressDialog* deploy_progress_ = nullptr;
    QThread* deploy_thread_ = nullptr;
};

}  // namespace ui
