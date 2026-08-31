#pragma once

#include "engine/core/instance/instance_utils.h"

#include <QWidget>

#include <filesystem>
#include <functional>
#include <string>

class QComboBox;
class QGroupBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QThread;
class QVBoxLayout;

namespace engine {
class Platform;
class PluginLoader;
} // namespace engine

namespace ui {

// Mode-agnostic Instance Options panel: per-instance runner selector plus the
// game's recommended wine packages (wine.json shipped with the game plugin).
// The runner is persisted to instance.toml by the host after save_requested()
// (QDialog::accept() in popup mode; writing the key + closing the tab in Full
// UI tab mode), following the SettingsDialog pattern.
//
// It also hosts a "Deploy management" section: a "Deployment strategy" dropdown
// (only the strategies the program + host actually support are listed) whose
// selection is persisted to instance.toml immediately on change, plus — for
// direct (symlink) deploys only — "Force re-deploy links" (tears down the
// current deploy, restoring any original game files parked in
// <game_dir>/Original_Files, then re-deploys all enabled mods) and "Remove
// deployed files" (teardown only, returning the game to its pristine unmodded
// state). The task buttons run on a background thread with an inline progress
// bar (THREADING.md: the widget's event loop keeps pumping, so queued
// progress/result callbacks are delivered; no engine call ever runs on the
// main thread). The inline bar replaces the modal QProgressDialog the popup
// used, so the same widget works embedded in a tab.
//
// The runner selector and the recommended packages live in a "Runtime
// Environment" group that is hidden for non-Steam games (steam_appid == 0),
// which have no Proton prefix to configure.
//
// Extracted from InstanceOptionsDialog so the same content can be embedded
// either in a popup QDialog (InstanceOptionsDialog) or as a tab page inside
// MainTabContainer (Full UI tab mode). The widget never applies the runner
// selection by itself: the bottom Save/Close buttons emit save_requested() /
// cancel_requested() and the host decides what saving means.
class InstanceOptionsWidget : public QWidget {
  Q_OBJECT
public:
  InstanceOptionsWidget(engine::Platform *platform,
                        engine::PluginLoader *plugin_loader,
                        const std::string &game_id,
                        const std::string &game_display_name,
                        const std::filesystem::path &game_dir,
                        uint32_t steam_appid,
                        const std::filesystem::path &instance_root,
                        const std::string &current_runner,
                        const std::string &current_deploy_strategy,
                        const engine::DeployConfig &deploy_config,
                        QWidget *parent = nullptr);

  ~InstanceOptionsWidget() override;

  // Runner selected in the widget (display name or absolute path).
  // Empty = automatic (Steam per-game override, then latest).
  [[nodiscard]] std::string selected_runner() const;

  // Installs the host's deferred disable/enable queue flush (delayed_disable
  // games). The widget itself is mode-agnostic and never touches MainWindow
  // internals; the host (LaunchController / TabModeController) supplies the
  // flush so the Force re-deploy / Remove actions apply queued toggles to the
  // on-disk sentinels BEFORE the deploy worker starts. Without this the deploy
  // would read stale sentinels and ignore toggles queued since the last Run.
  void set_flush_deferred_disable_queue(std::function<void()> flush);

signals:
  // Emitted when the user clicks Save. The host decides what saving means:
  // QDialog::accept() in popup mode, persisting the runner to instance.toml +
  // closing the tab in Full UI tab mode.
  void save_requested();
  // Emitted when the user clicks Close. The host decides what closing means:
  // QDialog::reject() in popup mode, closing the tab without persisting the
  // runner in Full UI tab mode.
  void cancel_requested();

private:
  enum class DeployTaskKind { Redeploy, Remove };

  void refresh_runners();
  void update_runner_detail();
  void load_recommended_packages();
  void install_packages(const QStringList &verbs);
  void build_deploy_management();
  void update_deploy_actions_enabled();
  void run_deploy_task(DeployTaskKind kind);
  void finish_deploy_task(DeployTaskKind kind, bool ok);
  [[nodiscard]] std::filesystem::path recommended_packages_path() const;

  engine::Platform *platform_ = nullptr;
  engine::PluginLoader *plugin_loader_ = nullptr;
  std::string game_id_;
  std::string game_display_name_;
  std::filesystem::path game_dir_;
  uint32_t steam_appid_ = 0;
  std::filesystem::path instance_root_;
  std::string current_deploy_strategy_;
  engine::DeployConfig deploy_config_;

  QComboBox *runner_combo_ = nullptr;
  QLabel *runner_detail_ = nullptr;
  QPushButton *install_all_btn_ = nullptr;
  QVBoxLayout *packages_layout_ = nullptr;
  QLabel *packages_status_ = nullptr;
  QComboBox *deploy_strategy_combo_ = nullptr;
  QPushButton *redeploy_btn_ = nullptr;
  QPushButton *remove_btn_ = nullptr;
  // Inline deploy-task progress (hidden while idle): replaces the modal
  // QProgressDialog so the widget works embedded in a tab.
  QProgressBar *deploy_progress_ = nullptr;
  QLabel *deploy_status_ = nullptr;
  QThread *deploy_thread_ = nullptr;
  bool deploy_task_running_ = false;
  // Host-supplied flush of the deferred disable/enable queue; empty when the
  // host did not install one (the deploy actions then behave as before).
  std::function<void()> flush_deferred_disable_queue_;
};

} // namespace ui
