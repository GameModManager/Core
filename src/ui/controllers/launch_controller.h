#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <filesystem>
#include <string>

#include "ui/main_window/main_window.h"

namespace ui {

// Game launching: executable list persistence, deploy-before-launch
// (DeployThread), process watch + game-lock overlay, "output to mod" capture,
// toolbar/desktop shortcuts, and Proton prefix tools. Split out of the
// 7211-line main_window.cpp (Issue #16).
class LaunchController : public QObject {
  Q_OBJECT
public:
  explicit LaunchController(MainWindow *w, QObject *parent = nullptr);

public slots:
  void save_executables();
  void load_executables();
  void populate_executables();
  void launch_game();
  void launch_with_executable(const QString &full_path,
                              const std::filesystem::path &output_mod_dir = {});
  // Resolves an output-to-mod target folder, auto-creating it (with the
  // game's metadata file) when it doesn't exist yet. Empty input -> empty.
  std::filesystem::path ensure_output_mod_dir(const QString &mod_name);
  void add_shortcut_to_toolbar();
  void add_toolbar_shortcut_from_path(const QString &full_path,
                                      const QString &icon_path = {});
  void add_shortcut_to_desktop();
  void on_add_entry_requested();
  static bool validate_linux_executable(const QString &path);
  void check_running_process();
  void on_deploy_progress(int files_done, int files_total);
  void do_capture_overwrite(std::filesystem::file_time_type capture_time);

  // Plain members (not slots): connected via function pointers. Leaving
  // on_launch_params_prepared out of the slots section keeps
  // moc_launch_controller.cpp from instantiating
  // QMetaTypeId<engine::LaunchParams> before deploy_worker.h declares it
  // (Q_DECLARE_METATYPE ordering in mocs_compilation.cpp).
public:
  void on_launch_params_prepared(engine::LaunchParams params);
  // Game-lock overlay
  void create_game_lock_overlay();
  void show_game_lock_overlay(const QString &binary_name, int64_t pid);
  void hide_game_lock_overlay();
  void refresh_process_tree();
  void copy_process_tree();
  // Proton tools
  void show_proton_panel();
  void run_prefix_tool(const QStringList &args);
  void run_exe_in_prefix();
  [[nodiscard]] engine::ProtonToolRequest current_proton_request() const;

private:
  MainWindow *w_ = nullptr;
};

} // namespace ui