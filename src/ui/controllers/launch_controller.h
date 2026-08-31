#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <filesystem>
#include <string>
#include <vector>

#include "engine/core/instance/instance_utils.h"
#include "ui/main_window/main_window.h"

namespace ui {

namespace Executables { struct Entry; }

// Snapshot of everything the Instance Options UI needs to construct itself,
// gathered once from the current instance + game knowledge (instance_utils:
// single source of truth for direct-symlink deploys). Shared by the popup
// path (show_instance_options) and the Full UI tab path
// (TabModeController::route_instance_options) so both embed the exact same
// InstanceOptionsWidget.
struct InstanceOptionsParams {
  std::string game_id;
  std::string game_name;
  std::filesystem::path game_dir;
  std::filesystem::path instance_root;
  uint32_t steam_appid = 0;
  std::string current_runner;
  std::string deploy_strategy;
  engine::DeployConfig deploy_config;
  // False when no instance is loaded (callers show the "no instance" info
  // box instead of constructing a panel).
  bool valid = false;
};

// Parses the `executables` array from instance.toml content and returns each
// entry as a JSON string (the format consumed by ExecControlsBar). Handles
// TOML inline tables and legacy plain-string entries; legacy JSON-style
// inline tables ({"path":"..."}) are repaired by
// engine::parse_instance_toml_content. Empty when the key is missing or the
// content is unparseable. Backed by toml++ (Issue #5e8) — the pre-toml++
// bracket-depth scan (Issue #34) is superseded by a real TOML parser.
std::vector<std::string> extract_executables(const std::string &content);

// First-launch seed list for the executables combo (Workspace-421). Keeps the
// plugin-declared names that physically exist under game_dir (Workspace-6su),
// then scans `extra_roots` with the same declarations - on macOS these are
// ~/Applications and /Applications, where Steam can install the game's .app
// bundle outside game_dir. Extra-root hits are appended as ABSOLUTE paths:
// every consumer resolves `game_dir / entry.path`, which an absolute path
// bypasses by design (legacy pre-#34 entries worked the same way), so launch,
// icons and shortcuts reach the bundle with no new plumbing. Basename-dedupe
// keeps the game_dir hit when both copies exist. Free function so it tests
// without a full MainWindow; roots are parameters so the merge is testable on
// any platform.
std::vector<std::string> seed_executable_candidates(
    const std::filesystem::path &game_dir, const std::string &declared,
    const std::vector<std::filesystem::path> &extra_roots);

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
  // Launch an executable with the full Executables::Entry configuration.
  // `output_mod_dir` is the explicit output-to-mod target (empty = resolve from
  // the entries / Overwrite capture); `arguments` / `start_in` / `environment`
  // come from the referenced Executables::Entry and are applied to the launched process
  // (Issue #34).
  void launch_with_executable(const QString &full_path,
                              const std::filesystem::path &output_mod_dir = {},
                              const QString &arguments = {},
                              const QString &start_in = {},
                              const QStringList &environment = {});
  // Resolves an output-to-mod target folder, auto-creating it (with the
  // game's metadata file) when it doesn't exist yet. Empty input -> empty.
  std::filesystem::path ensure_output_mod_dir(const QString &mod_name);
  void add_shortcut_to_toolbar();
  // Adds a toolbar shortcut referencing an executable by game-relative path.
  // `legacy_icon` is only used when restoring a pre-#34 instance.toml that
  // stored per-shortcut icons (the schema now inherits the icon from the
  // referenced Executables::Entry); new shortcuts pass an empty icon.
  void add_toolbar_shortcut_from_path(const QString &rel_path,
                                      const QString &legacy_icon = {});
  // Launches the executable referenced by a toolbar shortcut. Resolves the
  // full Executables::Entry configuration (args, cwd, env, output mod, icon) at click
  // time so shortcuts stay in sync with the executables list.
  void launch_toolbar_shortcut(const QString &rel_path);
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
  // Builds the "Output to mod" combo list from the current mod model
  // (id, display name) for the executable editor. Shared by the popup dialog
  // and the Full UI tab editor.
  QVector<QPair<QString, QString>> output_mod_list() const;
  // Applies a full entry set to the executables combo: replaces the combo
  // content, restores the previous selection and persists. Shared by the
  // popup dialog accept and the Full UI tab Save.
  void apply_exec_entries(const QVector<Executables::Entry> &entries);
  // Game-lock overlay
  void create_game_lock_overlay();
  void show_game_lock_overlay(const QString &binary_name, int64_t pid);
  void hide_game_lock_overlay();
  void refresh_process_tree();
  void copy_process_tree();
  // Proton tools
  void show_instance_options();
  void run_prefix_tool(const QStringList &args);
  void run_exe_in_prefix();
  [[nodiscard]] engine::ProtonToolRequest current_proton_request() const;
  // Builds the InstanceOptionsParams snapshot for the current instance.
  // `valid` is false when no instance is loaded.
  [[nodiscard]] InstanceOptionsParams instance_options_params() const;
  // Flushes the deferred disable/enable queue (delayed_disable games) by
  // writing/removing the on-disk disable sentinel for every queued mod. Runs
  // synchronously on the UI thread in launch_with_executable BEFORE the
  // DeployWorker starts, so the deploy (which reads on-disk sentinels) sees
  // the reconciled state. Also invoked by the Instance Options deploy
  // management (Force re-deploy / Remove) before its deploy worker starts, so
  // a queued toggle followed by Force re-deploy is not ignored. Clears the
  // queue after the flush.
  void flush_deferred_disable_queue();

private:
  // Migration + materialization for toolbar shortcuts (Issue #34): after the
  // executables combo is populated, every pinned path that has no matching
  // Executables::Entry gets a minimal path-only entry materialized so the reference
  // resolves and stays editable; legacy per-shortcut icons recorded by
  // add_toolbar_shortcut_from_path are folded into the referenced entry.
  void materialize_toolbar_shortcuts();
  // Writes a shell wrapper (under the instance cache) that exports the
  // entry's environment variables and execs the real command with the
  // working directory. Used by add_shortcut_to_desktop for entries with env
  // vars - a .desktop Exec= line cannot set environment variables. Returns
  // the wrapper path, or empty on failure. `base_name` keys the wrapper
  // filename so it matches the .desktop file it belongs to.
  QString write_desktop_wrapper(const Executables::Entry &entry,
                                const std::filesystem::path &exec_path,
                                const std::filesystem::path &work_dir,
                                const QString &base_name);
  MainWindow *w_ = nullptr;
  // Transient legacy-icon handoff between load_order() (toolbar restore runs
  // before load_executables/populate_executables) and materialization. Keyed
  // by the game-relative path; cleared after populate_executables.
  QHash<QString, QString> pending_toolbar_icons_;
};

} // namespace ui