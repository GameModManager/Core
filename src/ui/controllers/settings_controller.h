#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <filesystem>
#include <string>

#include "ui/main_window/main_window.h"

namespace ui {

// App/instance shell: loading a game instance (set_game_info), app-state
// persistence, the settings dialog, instance switcher/statistics, pipeline
// window, NXM protocol registration, and the menu bar wiring. Split out of the
// 7211-line main_window.cpp (Issue #16).
class SettingsController : public QObject {
  Q_OBJECT
public:
  explicit SettingsController(MainWindow *w, QObject *parent = nullptr);

  // Builds the menu bar and connects its actions to the controllers. Called
  // from the MainWindow ctor (the composer owns the toolbar; the wiring
  // lives here so main_window.cpp stays under 300 lines).
  void setup_menu_bar();
  void connect_menu_actions();

public slots:
  void set_game_info(const std::string &game_id,
                     const std::string &game_display_name,
                     const std::string &profile_name = "Default",
                     const std::filesystem::path &game_dir = {},
                     const std::filesystem::path &instance_root = {});
  void save_app_state();
  void restore_app_state();
  QJsonObject read_app_state_extra() const;
  void restore_exec_selection();
  void show_settings_dialog();
  // Re-apply settings that may have changed in the settings dialog (icons,
  // nesting, scrollbar policy, compact downloads, nexus queue). Called after
  // the dialog closes in popup mode and after the settings tab closes in
  // Full UI tab mode.
  void apply_settings_changes();
  // Apply the per-instance "Nested mod list" setting to the model. Called on
  // scan finish (before load_order so folds/links render correctly) and after
  // the settings dialog closes.
  void apply_nesting_setting();
  void show_instance_statistics();
  void show_pipeline_window();
  void show_instance_switcher();
  bool switch_to_instance(const QString &name);
  // Runs the "Create new instance" flow: detects installed games, shows the
  // GameSelectionWidget picker, creates the instance and switches to it.
  // Returns true when an instance was created and loaded, false when the user
  // cancelled the picker or creation failed. Shared by the popup switcher
  // (after the dialog's create button) and the Full UI tab-mode switcher.
  bool create_new_instance();
  void refresh_recent_instances();
  void prompt_nxm_registration();
  void ensure_nxm_handler_default();

  // Global event filter (installed on QApplication by MainWindow): the Konami
  // code easter egg toggles the debug window. Returns true when the event was
  // consumed; MainWindow::eventFilter falls through to QMainWindow otherwise.
  bool handle_global_event(QObject *obj, QEvent *event);

private:
  MainWindow *w_ = nullptr;
};

} // namespace ui