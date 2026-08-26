#pragma once

#include <QByteArray>
#include <QHash>
#include <QMainWindow>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVector>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "engine/deploy/strategy.h"
#include "engine/index/conflict_engine.h"
#include "engine/core/instance/instance.h"
#include "engine/mod/meta/mod_meta.h"
#include "engine/source/nxm/nxm_router.h"
#include "engine/game/plugins/plugin_database.h"
#include "engine/deploy/launch/proton_tools.h"
#include "engine/game/registry/game_knowledge.h"
#include "engine/profile/profile.h"
#include "platform/platform_interface.h"

class QSplitter;
class QToolBar;
class QMenu;
class QKeyEvent;
class QLabel;
class QLCDNumber;
class QPushButton;
class QCheckBox;
class QTreeWidget;
class QTimer;

namespace engine {
class GameKnowledge;
class PluginLoader;
class ManagedGames;
class NxmIpcServer;
struct NxmLink;
struct ConflictStats;
class StyleManager;
class PlatformInterface;
struct LootResult;
struct LaunchParams;
} // namespace engine

namespace ui {

class DebugWindow;
class ModListModel;
class ModTableView;
class ColumnToggleHeaderView;
class MainToolbar;
class ProfileBar;
class ModFilterBar;
class CategoryFilterPanel;
class RightPanel;
class MainTabContainer;
class DeployThread;
class GamePathBanner;

// Issue #16 controller split: the controllers own the behavior that used to
// live in the 7211-line main_window.cpp; MainWindow is the composer (<300
// lines) that owns the widgets and connects the controllers. They are friends
// so they can reach the shared members below through w_->.
class ModListController;
class LaunchController;
class OverwriteController;
class QueueController;
class SettingsController;
class DownloadsController;
class TabModeController;

// Forward-declared: fully defined in ui/widgets/profile_bar.h, which owns the
// FolderKind enum and is included before any use in .cpp files.
enum class FolderKind;
class ConsolePanel;
class GmmStatusBar;
class PipelineThread;
class AppMenuBar;
class PipelineWindow;
class PluginsTab;
class DataTab;
class ModInfoDialog;
class InstallProgressDialog;
class LootSortThread;
class ConflictScanThread;
class ModScanThread;
class PluginDbLoadThread;
struct ModInfoData;
struct ModEntry;
struct ConflictScanResult;
struct ConflictScanRequest;
struct ModScanResult;
struct ModScanRequest;
struct PluginDbLoadRequest;
namespace preview {
class PreviewWindow;
}

struct PendingToggle {
  QString mod_id;
  bool enabled = false;
};

// One deferred disable/enable operation for a delayed_disable game (e.g.
// Isaac's Direct deploy mode). The engine skips the immediate on-disk sentinel
// write on toggle and records the desired state here instead; the queue is
// flushed synchronously in launch_with_executable before the deploy worker
// starts, so the deploy (which reads on-disk sentinels) sees the reconciled
// state. Latest-wins per mod_id; a profile switch queues the FULL desired
// profile state (idempotent, self-healing).
struct DeferredDisable {
  std::string mod_id;
  bool enabled = false;
};

class MainWindow : public QMainWindow {
  Q_OBJECT
public:
  explicit MainWindow(QWidget *parent = nullptr);
  // Out-of-line so the unique_ptr controller members (ModListController,
  // SettingsController, ...) are destroyed where their complete types are
  // visible (main_window.cpp includes every controller header).
  ~MainWindow() override;

  void set_game_info(const std::string &game_id,
                     const std::string &game_display_name,
                     const std::string &profile_name = "Default",
                     const std::filesystem::path &game_dir = {},
                     const std::filesystem::path &instance_root = {});

  // Opens the game-directory picker, persists the choice to instance.toml
  // (read-before-write) and reloads through set_game_info. Returns true
  // when a game dir is now set. Shared by the banner (Workspace-tnj) and
  // the launch/deploy guards (Workspace-wk8).
  bool prompt_for_game_path();

  void set_game_knowledge(engine::GameKnowledge *knowledge) {
    knowledge_ = knowledge;
  }
  void set_plugin_loader(engine::PluginLoader *loader) {
    plugin_loader_ = loader;
  }
  void set_managed_games(engine::ManagedGames *mg) { managed_games_ = mg; }
  void set_style_manager(engine::StyleManager *sm) { style_manager_ = sm; }
  void set_platform(engine::PlatformInterface *platform) {
    platform_ = platform;
  }

  // The QApplication's initial (native platform) style name, captured before
  // any user-selected style is applied. Used to restore "Default (system)"
  // after a built-in Qt style was picked in Settings.
  void set_native_style_name(const QString &name) { native_style_name_ = name; }

  // NXM download routing - call when an nxm:// link is received. Delegates
  // to the DownloadsController (Issue #16); kept on MainWindow as the
  // public entry point used by main.cpp.
  void handle_nxm_download(const engine::NxmLink &link);

  [[nodiscard]] ModTableView *mod_view() const { return mod_view_; }
  [[nodiscard]] QSplitter *console_splitter() const {
    return console_splitter_;
  }

  // True while a mod scan / install pipeline stage owns the UI state
  // (loading_ flag). Tests and controllers use it to defer disk-effect
  // assertions until the async instance scan has landed.
  [[nodiscard]] bool is_loading() const { return loading_; }

  // Lock or unlock the whole mod manager interface. Locking blocks user
  // interaction (mouse + keyboard) across the main window and greys it out,
  // so long-running operations like archive installs can't race the user.
  // Always re-enable when the operation finishes or cancels - every path
  // that disables must re-enable on completion/failure.
  void set_ui_enabled(bool enabled);

  // Apply saved window geometry after show() - needed for Wayland where
  // restoreGeometry() before platform-window creation is silently ignored.
  void apply_initial_geometry();

protected:
  void closeEvent(QCloseEvent *event) override;
  bool eventFilter(QObject *obj, QEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;

private slots:
  void on_notification(const QString &title, const QString &message);

private:
  void update_title();

  // --- Shared state owned by the composer, used by the controllers ---
  AppMenuBar *menu_bar_ = nullptr;
  QToolBar *toolbar_area_ = nullptr;
  MainToolbar *toolbar_ = nullptr;
  ProfileBar *profile_bar_ = nullptr;
  ModFilterBar *filter_bar_ = nullptr;
  // MO2-style category filter panel (checkable category tree + Clear/Edit).
  // Hidden by default; the << / >> toggle in the filter bar shows/hides it.
  CategoryFilterPanel *category_filter_panel_ = nullptr;
  ModTableView *mod_view_ = nullptr;
  // MO2-style digital counter above the mod list (enabled mod count),
  // right-aligned; updated by ModListController on list/toggle changes.
  QLCDNumber *mod_count_enabled_ = nullptr;
  ColumnToggleHeaderView *mod_header_ = nullptr;
  ModListModel *mod_model_ = nullptr;
  RightPanel *right_panel_ = nullptr;
  QSplitter *main_splitter_ = nullptr;
  QSplitter *console_splitter_ = nullptr;
  // Central widget: tab 0 holds console_splitter_ (the Main tab); dynamic
  // view tabs (Settings, Pipeline, ...) are added by TabModeController when
  // Full UI mode is ON.
  MainTabContainer *main_tab_container_ = nullptr;
  ConsolePanel *console_ = nullptr;
  GmmStatusBar *status_bar_ = nullptr;
  // "Set Game Path" banner (Workspace-tnj): visible while a game-less
  // instance is loaded; lives at the top of the main area.
  GamePathBanner *game_path_banner_ = nullptr;
  PipelineThread *pipeline_thread_ = nullptr;
  engine::GameKnowledge *knowledge_ = nullptr;
  engine::PluginLoader *plugin_loader_ = nullptr;
  engine::ManagedGames *managed_games_ = nullptr;
  QString native_style_name_;
  engine::StyleManager *style_manager_ = nullptr;
  engine::PlatformInterface *platform_ = nullptr;
  engine::NxmIpcServer *nxm_ipc_ = nullptr;
  std::unique_ptr<engine::DeploymentStrategy> deploy_strategy_;
  bool nxm_handler_check_done_ = false;

  std::string current_game_id_;
  std::string current_game_name_;
  std::string current_profile_name_ = "Default";
  std::filesystem::path current_game_dir_;
  std::filesystem::path current_instance_root_;
  bool loading_ = false;
  // The active profile's engine model (modlist.txt state). Owned by the
  // window; created on instance load, replaced on profile switch. The UI's
  // ModListModel is converged with it after every scan
  // (on_mod_scan_finished) and every toggle (sync_mod_enable_state), so the
  // profile's modlist.txt is the per-profile source of truth for enabled
  // state — never the global on-disk disable.it marker.
  std::unique_ptr<engine::profile::Profile> active_profile_;
  // Plugin database driving the Plugins tab (empty until a plugin-capable
  // game is loaded). Rebuilt on refresh; toggles/moves save the profile.
  engine::PluginDatabase plugins_db_;
  ui::PluginsTab *plugins_tab_widget_ = nullptr;
  // Long-lived LOOT sort worker thread (created on first use, reused).
  ui::LootSortThread *loot_sort_thread_ = nullptr;
  // Data tab context-menu targets. data_tab_widget_ is set by set_game_info()
  // after each right-panel rebuild; preview_window_ is lazily created on the
  // first preview request and kept across rebuilds.
  ui::DataTab *data_tab_widget_ = nullptr;
  ui::preview::PreviewWindow *preview_window_ = nullptr;
  // Selection-highlight indexes, rebuilt once per plugin refresh (O(P)); the
  // per-selection work is then lookups only, so huge mod lists stay cheap.
  // owner_mod -> plugin names the mod owns; name -> row in plugins_db_.
  QHash<QString, QVector<QString>> plugin_owner_index_;
  QHash<QString, int> plugin_row_by_name_;
  // Toolbar shortcut pins: game-relative executable paths referencing the
  // executables list (Issue #34). The icon, args/cwd/env, output mod and
  // title are inherited from the referenced ExecEntry at click time.
  QStringList toolbar_shortcut_paths_;
  std::vector<std::string> saved_executables_;
  std::string pending_nxm_url_;
  // In-flight/known Nexus downloads keyed by "<mod_id>-<file_id>", kept so a
  // paused download can be resumed with its original NXM link.
  std::unordered_map<std::string, engine::NxmLink> nxm_links_;
  // In-flight/known LoversLab downloads keyed by the download id, kept so a
  // paused download can be resumed with its original ?do=download URL.
  std::unordered_map<std::string, std::string> url_downloads_;
  int64_t running_process_pid_ = -1;
  QTimer *process_watch_timer_ = nullptr;
  bool overlay_launched_ = false;
  std::string
      cgroup_path_; // cgroup v2 path for process tracking (empty = unavailable)
  std::filesystem::file_time_type launch_time_;
  std::filesystem::path
      staging_dir_; // non-empty when OverlayFS deploy strategy is active
  // "Output to mod" session: scratch capture dir + target mod folder.
  // Both empty = default Overwrite capture.
  std::filesystem::path output_session_scratch_;
  std::filesystem::path output_mod_dir_;
  std::filesystem::path conflict_cache_path_; // path to conflict cache JSON
  engine::PathRegistry last_conflict_registry_;
  // Conflict recompute machinery (P8.1): debounce timer coalesces rapid
  // toggle/reorder requests; the scan runs on ConflictScanThread with at most
  // one in flight (conflict_scan_running_); requests arriving mid-scan queue
  // a fresh scan (conflict_scan_pending_); generation drops stale results;
  // invalidations of the quick-token cache are applied by the worker before
  // it scans. Follow-ups run on the main thread after the results land.
  QTimer *conflict_debounce_timer_ = nullptr;
  ui::ConflictScanThread *conflict_scan_thread_ = nullptr;
  bool conflict_scan_running_ = false;
  bool conflict_scan_pending_ = false;
  quint64 conflict_scan_generation_ = 0;
  std::unordered_set<std::string> conflict_invalidate_pending_;
  std::vector<std::function<void()>> conflict_scan_pending_follow_ups_;
  std::vector<std::function<void()>> conflict_scan_active_follow_ups_;
  // Mod scan machinery (P8.2): load_mods_from_game() launches the scan on
  // ModScanThread; generation drops a stale result (refresh or instance
  // switch superseded it). No reentrancy flag needed — the model is only
  // touched from on_mod_scan_finished on the main thread, and the worker
  // thread serializes queued scans.
  ui::ModScanThread *mod_scan_thread_ = nullptr;
  quint64 mod_scan_generation_ = 0;
  // Plugin-DB preload machinery (P8.5/T6): launch_plugin_db_preload() runs
  // the plugin-DB disk load concurrently with the mod scan on
  // PluginDbLoadThread (gmm-plugin-db). plugin_db_generation_ drops a stale
  // load (instance switch bumps it); preload_pending_ is true only between a
  // launch and its consumption — either adoption by refresh_plugins_tab() or
  // a synchronous fallback read (which discards the pending preload so it
  // can't land late and clobber fresher data).
  ui::PluginDbLoadThread *plugin_db_load_thread_ = nullptr;
  quint64 plugin_db_generation_ = 0;
  bool preload_pending_ = false;
  std::optional<engine::PluginDatabase> preloaded_plugin_db_;
  std::filesystem::path preloaded_plugin_db_game_dir_;
  // Launch deploy machinery (P8.4): launch_with_executable() builds a
  // LaunchPrepRequest snapshot and runs prepare_launch_params on DeployThread
  // (gmm-deploy); launch_game() only ever runs from on_launch_params_prepared
  // after the deploy finished, so the game provably never starts before
  // .gmm_staging is fully populated. launch_prep_pending_ re-entry-guards the
  // gap; a stale result (instance switched mid-deploy) is dropped.
  ui::DeployThread *launch_deploy_thread_ = nullptr;
  bool launch_prep_pending_ = false;
  engine::Instance
      current_instance_; // loaded per-folder overrides for the active instance
  QPointer<ui::ModInfoDialog> modinfo_dialog_; // alive while the dialog is open
  // --- Path helpers (inlined: the composer stays thin, Issue #16) ---
  std::filesystem::path meta_dir_path() const {
    if (current_instance_root_.empty())
      return {};
    return current_instance_root_ / "meta";
  }
  std::filesystem::path mods_dir_path() const {
    if (current_instance_root_.empty() && current_game_dir_.empty())
      return {};
    // MO2-style: mods managed from instance root (even if mods_subpath is set)
    if (!current_instance_root_.empty())
      return current_instance_.path_for(engine::InstanceKind::Mods);
    auto subpath =
        knowledge_ ? knowledge_->get(current_game_id_, "mods_subpath", "") : "";
    if (!subpath.empty())
      return current_game_dir_ / subpath;
    return current_game_dir_;
  }
  std::filesystem::path downloads_dir_path() const {
    if (current_instance_root_.empty())
      return {};
    return current_instance_.path_for(engine::InstanceKind::Downloads);
  }
  std::filesystem::path cache_dir_path() const {
    if (current_instance_root_.empty())
      return {};
    return current_instance_.path_for(engine::InstanceKind::Cache);
  }
  std::filesystem::path cache_thumbnails_dir_path() const {
    if (current_instance_root_.empty())
      return {};
    return current_instance_.path_for(engine::InstanceKind::CacheThumbnails);
  }
  std::filesystem::path profiles_dir_path() const {
    if (current_instance_root_.empty())
      return {};
    return current_instance_.path_for(engine::InstanceKind::Profiles);
  }
  std::filesystem::path overwrite_dir_path() const {
    if (current_instance_root_.empty())
      return {};
    return current_instance_.path_for(engine::InstanceKind::Overwrite);
  }
  // Game's My Games folder under the prefix Documents dir (MO2's
  // documentsDirectory). Empty when the game has no prefix / appid.
  std::filesystem::path game_mygames_dir() const {
    if (!platform_ || !knowledge_ || current_game_id_.empty())
      return {};
    auto id_str = knowledge_->get(current_game_id_, "steam_appid", "");
    if (id_str.empty())
      return {};
    uint32_t appid = 0;
    try {
      appid = std::stoul(id_str);
    } catch (...) {
      return {};
    }
    const auto documents = platform_->game_documents_dir(appid);
    if (documents.empty())
      return {};
    auto sub = knowledge_->get(current_game_id_, "mygames_folder", "");
    if (sub.empty())
      sub = current_game_name_.empty() ? current_game_id_ : current_game_name_;
    return documents / "My Games" / sub;
  }
  std::filesystem::path
  resolve_mod_folder(const std::string &mod_id,
                     const std::string &mods_subpath) const {
    auto folder = mods_dir_path() / mod_id;
    if (std::filesystem::exists(folder))
      return folder;
    if (!mods_subpath.empty()) {
      auto fallback = current_game_dir_ / mods_subpath / mod_id;
      if (std::filesystem::exists(fallback))
        return fallback;
    }
    return folder; // return the instance path even if it doesn't exist
  }
  // Game-native mods dir: the instance.toml "game_mods_dir" override when
  // set (Workspace-6up), else the plugin-declared "game_mods_dir" hook or
  // mods_subpath under the game dir (Workspace-otx resolution chain). Empty
  // when it equals the instance mods dir or no game is loaded.
  std::filesystem::path current_game_mods_dir() const {
    std::filesystem::path game_mods_dir;
    if (knowledge_)
      game_mods_dir = engine::resolve_game_mods_dir(
          current_game_id_, current_game_dir_, *knowledge_,
          current_instance_.info().game_mods_dir.string());
    // Only pass as extra dir if it differs from the instance mods dir
    if (!game_mods_dir.empty() && game_mods_dir == mods_dir_path())
      game_mods_dir.clear();
    return game_mods_dir;
  }
  std::filesystem::path app_state_path() const {
    if (current_instance_root_.empty())
      return {};
    return current_instance_root_ / "config" / "app_state.dat";
  }
  std::filesystem::path download_manifest_path() const {
    if (current_instance_root_.empty())
      return {};
    return downloads_dir_path() / ".download_manifest.json";
  }
  QByteArray pending_geometry_;
  // Restored app state, applied once the widgets are ready
  int icon_size_ = 24; // toolbar icon size (small/medium/large)
  QString
      pending_exec_selection_; // last selected executable path, per instance

  // Konami code easter egg
  DebugWindow *debug_window_ = nullptr;
  PipelineWindow *pipeline_window_ = nullptr;
  int konami_state_ = 0;
  static constexpr int konami_sequence_[10] = {
      Qt::Key_Up,    Qt::Key_Up,   Qt::Key_Down,  Qt::Key_Down, Qt::Key_Left,
      Qt::Key_Right, Qt::Key_Left, Qt::Key_Right, Qt::Key_B,    Qt::Key_A};

  // Pending changes queue (deferred until game exits)
  std::vector<PendingToggle> pending_changes_;
  QLabel *pending_queue_label_ = nullptr;

  // Deferred disable/enable queue for delayed_disable games (see
  // DeferredDisable above). Distinct from pending_changes_: this queue is
  // flushed at the next Run (launch_with_executable), never at game exit.
  std::vector<DeferredDisable> deferred_disable_queue_;

  // Game-lock overlay
  QWidget *game_lock_overlay_ = nullptr;
  QLabel *game_lock_label_ = nullptr;
  QPushButton *unlock_button_ = nullptr;
  QPushButton *kill_button_ = nullptr;
  QCheckBox *process_tree_checkbox_ = nullptr;
  QTreeWidget *process_tree_ = nullptr;
  int64_t locked_pid_ = -1;
  bool show_process_tree_ = false;

  // Install-progress popup state. Lazily created on the first install;
  // kept across installs within the session. install_progress_show_timer_
  // defers the first show by ~300ms so a fast install never flashes the
  // dialog; active_install_progress_id_ tracks which install the dialog
  // belongs to so a new install resets it.
  ui::InstallProgressDialog *install_progress_dialog_ = nullptr;
  QTimer *install_progress_show_timer_ = nullptr;
  std::string active_install_progress_id_;

  // Issue #16 controllers — the composer delegates behavior to these.
  friend class ModListController;
  friend class LaunchController;
  friend class OverwriteController;
  friend class QueueController;
  friend class SettingsController;
  friend class DownloadsController;
  friend class TabModeController;
  std::unique_ptr<ModListController> mod_list_;
  std::unique_ptr<LaunchController> launch_;
  std::unique_ptr<OverwriteController> overwrite_;
  std::unique_ptr<QueueController> queue_;
  std::unique_ptr<SettingsController> settings_;
  std::unique_ptr<DownloadsController> downloads_;
  // Full-UI tab host (central widget) and its mode router. Created with the
  // other controllers; the Main tab is added once the console splitter
  // exists.
  std::unique_ptr<TabModeController> tab_mode_;
};

} // namespace ui
