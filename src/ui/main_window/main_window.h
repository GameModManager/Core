#pragma once

#include <QMainWindow>
#include <QByteArray>
#include <QHash>
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

#include "engine/index/conflict_engine.h"
#include "engine/meta/mod_meta.h"
#include "engine/deploy/strategy.h"
#include "engine/nxm/nxm_router.h"
#include "engine/instance/instance.h"
#include "engine/plugins/plugin_database.h"
#include "engine/proton_tools.h"

class QSplitter;
class QToolBar;
class QMenu;
class QKeyEvent;
class QLabel;
class QPushButton;
class QCheckBox;
class QTreeWidget;
class QTimer;

namespace engine {
class GameKnowledge;
class PluginLoader;
class ManagedGames;
class NxmIpcServer;
struct NxmLink;struct ConflictStats;
class StyleManager;
class PlatformInterface;
struct LootResult;
struct LaunchParams;
}

namespace ui {

class DebugWindow;
class ModListModel;
class ModTableView;
class MainToolbar;
class ProfileBar;
class ModFilterBar;
class RightPanel;
class DeployThread;

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
namespace preview { class PreviewWindow; }

struct PendingToggle {
    QString mod_id;
    bool enabled = false;
};

struct SourceVisitInfo {
    QString label;
    QString url;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

    void set_game_info(const std::string& game_id,
                       const std::string& game_display_name,
                       const std::string& profile_name = "Default",
                       const std::filesystem::path& game_dir = {},
                       const std::filesystem::path& instance_root = {});

    void set_game_knowledge(engine::GameKnowledge* knowledge) { knowledge_ = knowledge; }
    void set_plugin_loader(engine::PluginLoader* loader) { plugin_loader_ = loader; }
    void set_managed_games(engine::ManagedGames* mg) { managed_games_ = mg; }
    void set_style_manager(engine::StyleManager* sm) { style_manager_ = sm; }
    void set_platform(engine::PlatformInterface* platform) { platform_ = platform; }

    // The QApplication's initial (native platform) style name, captured before
    // any user-selected style is applied. Used to restore "Default (system)"
    // after a built-in Qt style was picked in Settings.
    void set_native_style_name(const QString& name) { native_style_name_ = name; }

    // NXM download routing - call when an nxm:// link is received
    void handle_nxm_download(const engine::NxmLink& link);

    // LoversLab download routing - call when the user pastes a
    // ?do=download link (LoversLab has no API; the session cookie is sent
    // by LoversLabProvider).
    void start_loverslab_download(const std::string& url);

    [[nodiscard]] ModTableView* mod_view() const { return mod_view_; }
    [[nodiscard]] QSplitter* console_splitter() const { return console_splitter_; }

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
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void on_notification(const QString& title, const QString& message);

private:
    void update_title();
    void setup_menu_bar();
    void connect_menu_actions();
    void setup_mod_list_context_menu();
    void load_mods_from_game();
    void add_installed_mod(const std::string& folder_name);
    void update_status_bar_for_game();
    void sync_mod_enable_state(const QString& mod_id, bool enabled);
    void sync_priorities();
    void sort_mods();
    void create_separator();
    QString create_separator_named(const QString& name, const QString& color);
    void create_empty_mod();
    void import_archives(const QStringList& paths);
    void export_modlist();
    void import_modlist();
    void open_folder(ui::FolderKind kind);
    void create_separator_at_row(int row);
    void rename_mod_inline(int row);          // start inline edit on a row's name cell
    void apply_rename(int row, const QString& name);  // model rename_requested handler
    void delete_separator(int row);
    void select_color_for_selected();
    void reset_color_for_selected();
    void save_order();
    void load_order();
    void save_executables();
    void load_executables();
    void sync_separator_ids();
    void group_mods_by_separator();
    void populate_executables();
    void launch_game();
    void launch_with_executable(const QString& full_path,
                                const std::filesystem::path& output_mod_dir = {});
    // Resolves an output-to-mod target folder, auto-creating it (with the
    // game's metadata file) when it doesn't exist yet. Empty input -> empty.
    std::filesystem::path ensure_output_mod_dir(const QString& mod_name);
    void add_shortcut_to_toolbar();
    void add_toolbar_shortcut_from_path(const QString& full_path,
                                         const QString& icon_path = {});
    void add_shortcut_to_desktop();
    void show_instance_switcher();
    bool switch_to_instance(const QString& name);
    void refresh_recent_instances();
    void on_add_entry_requested();
    static bool validate_linux_executable(const QString& path);
    void check_running_process();
    void on_deploy_progress(int files_done, int files_total);
    void on_launch_params_prepared(engine::LaunchParams params);
    void apply_mod_filter();
    void do_capture_overwrite(std::filesystem::file_time_type capture_time);
    void flush_pending_nxm();
    void flush_pending_changes();
    void update_queue_label();
    void prompt_nxm_registration();
    void ensure_nxm_handler_default();
    // Conflict recompute (THREADING.md §3.6, P8.1): recompute_conflicts() is
    // now a debounce entry point — rapid toggles/reorders coalesce into one
    // scan (single-shot QTimer). The scan itself runs off the main thread
    // (ConflictScanThread) on a snapshot of the mod list; results are applied
    // back to the model + registry on the main thread. request_conflict_scan()
    // is the immediate (non-debounced) entry for the install path; its
    // follow_up runs on the main thread once the results have landed.
    void recompute_conflicts();
    void request_conflict_scan(std::function<void()> follow_up);
    void start_conflict_scan();
    void on_conflict_scan_finished(ui::ConflictScanResult result, quint64 generation);
    void apply_conflict_results(const ui::ConflictScanResult& result);
    void launch_conflict_scan_batch(std::vector<std::function<void()>> follow_ups);
    ui::ConflictScanRequest build_conflict_scan_request();
    void reload_open_modinfo_dialog();
    // LOOT advisory-tool sort (PLAN.md §7.1): builds a LootRequest from the
    // current plugin DB, runs gmm_lootcli off the UI thread, applies the
    // sorted order, and persists it through save_profile().
    void run_loot_sort();
    void on_loot_progress(int stage, const QString& message);
    void on_loot_finished(engine::LootResult result);
    void refresh_data_tab();
    // Game-native mods dir (mods_subpath under the game dir), empty when it
    // equals the instance mods dir or no game is loaded.
    std::filesystem::path current_game_mods_dir() const;
    void wire_data_tab();
    void on_data_open(const QString& file_path);
    void on_data_execute(const QString& file_path, bool is_windows_exe,
                         const QString& vfs_path);
    void on_data_preview(const QString& file_path,
                         const QStringList& provider_paths,
                         const QStringList& provider_names);
    void on_data_add_executable(const QString& file_path, const QString& default_name,
                                const QString& physical_path = {});
    void on_data_mod_info(const QString& mod_id, int initial_tab = -1);
    void on_data_hide(const QString& file_path, const QString& mod_id, bool hide);
    ui::ModInfoData build_mod_info_data(const ModEntry& mod);
    void on_image_diff_requested(const QString& relative_path);
    // Mod scan (THREADING.md §3.5/§3.6, P8.2): load_mods_from_game() kicks a
    // scan onto ModScanThread (worker does all directory walking: ModScanner
    // scan/scan_dir, native/stray plugin synthesis, one-time MO2 meta import)
    // against a snapshot; the result lands here on the main thread, which then
    // rebuilds the model + meta + priorities. A generation counter drops a
    // stale scan's result when a refresh or instance switch superseded it.
    void on_mod_scan_finished(ui::ModScanResult result, quint64 generation);
    ui::ModScanRequest build_mod_scan_request();
    // Plugin-DB preload (THREADING.md §3.5, P8.5/T6): launch_plugin_db_preload()
    // runs the plugin-DB disk load (refresh -> header parse -> creation club ->
    // load order) on PluginDbLoadThread CONCURRENTLY with the mod scan, so the
    // two independent startup loads overlap instead of running sequentially.
    // The result lands in on_plugin_db_preloaded() (generation drops a stale
    // load after an instance switch) and is adopted by refresh_plugins_tab() —
    // which otherwise does the same disk read synchronously.
    void launch_plugin_db_preload();
    void on_plugin_db_preloaded(engine::PluginDatabase db, quint64 generation);
    bool adopt_preloaded_plugin_db();
    void load_meta_for_mods();
    void show_instance_statistics();
    void show_settings_dialog();
    void show_proton_panel();
    void show_pipeline_window();

    // Install-progress popup (MO2 parity): shows ~300ms into an install so
    // quick installs never flash it, hides before each interactive install
    // dialog (FOMOD wizard / name confirm / overwrite), and closes on install
    // completion/cancel/failure. update_install_progress is connected to
    // PipelineWorker::install_progress (queued onto this thread).
    void update_install_progress(const std::string& mod_id, int percent,
                                 const std::string& status);
    void hide_install_progress();

    // Run a wine/protontricks tool (winecfg, regedit, dlls, winetricks verbs,
    // recommended packages...) in the active instance's prefix.
    void run_prefix_tool(const QStringList& args);
    // Pick an .exe and run it in the active instance's prefix.
    void run_exe_in_prefix();
    // Build the ProtonToolRequest for the active instance (empty when none).
    [[nodiscard]] engine::ProtonToolRequest current_proton_request() const;

    // Plugins tab (Skyrim-style games with plugin support).
    void refresh_plugins_tab();
    void on_plugin_toggle(const std::string& name, bool enabled);
    void on_plugin_reorder(int from_row, int to_row);
    void on_plugin_lock(const std::string& name, bool locked);
    // Bidirectional selection highlighting (MO2 parity): mod selection marks
    // the plugins that mod owns (plugin_list_contained); plugin selection
    // marks the owning mods in the mod list (modlist_contains_file) and the
    // selected plugins' masters (plugin_list_master).
    void on_mod_selection_changed();
    void on_plugin_selection_changed();
    void rebuild_plugin_highlight_index();

    // Context menu helpers
    void clear_overwrite();
    void create_mod_from_overwrite();
    void move_overwrite_content_to_mod();
    void sync_overwrite_to_mods();
    void open_overwrite_in_file_manager();
    void show_overwrite_info_dialog();
    void move_dropped_overwrite_files(const QStringList& paths, int mod_row);
    void remove_selected_mods();
    void move_to_separator(const QString& mod_id, const QString& sep_id);
    void send_to_highest_priority(const QString& id);
    void send_to_lowest_priority(const QString& id);
    void send_to_highest_in_separator(const QString& id);
    void send_to_lowest_in_separator(const QString& id);
    void priority_move_selected(int step);
    void toggle_selected_mods(bool enabled);
    // "Treat mod as root dir" (Tweaks menu): persist [General] rootOverride in
    // each mod's meta.ini and refresh the model + Data tab. rows are model rows.
    void toggle_root_override(const QList<int>& rows, bool on);
    void open_source_for_mod(const QString& source_type, const QString& source_id);
    [[nodiscard]] SourceVisitInfo source_visit_info(const QString& source_type, const QString& source_id, const QString& page_url = {}) const;
    // Nexus game domain for the current game ("skyrimspecialedition"), resolved
    // from the loaded plugin's identity - the single source of truth (there is
    // NO "nexus_domain" knowledge hook; plugins register it via register_identity).
    [[nodiscard]] QString current_nexus_domain() const;

    // Game-lock overlay
    void create_game_lock_overlay();
    void show_game_lock_overlay(const QString& binary_name, int64_t pid);
    void hide_game_lock_overlay();
    void refresh_process_tree();
    void copy_process_tree();

    AppMenuBar* menu_bar_ = nullptr;
    QToolBar* toolbar_area_ = nullptr;
    MainToolbar* toolbar_ = nullptr;
    ProfileBar* profile_bar_ = nullptr;
    ModFilterBar* filter_bar_ = nullptr;
    ModTableView* mod_view_ = nullptr;
    ModListModel* mod_model_ = nullptr;
    RightPanel* right_panel_ = nullptr;
    QSplitter* main_splitter_ = nullptr;
    QSplitter* console_splitter_ = nullptr;
    ConsolePanel* console_ = nullptr;
    GmmStatusBar* status_bar_ = nullptr;
    PipelineThread* pipeline_thread_ = nullptr;
    engine::GameKnowledge* knowledge_ = nullptr;
    engine::PluginLoader* plugin_loader_ = nullptr;
    engine::ManagedGames* managed_games_ = nullptr;
    QString native_style_name_;
    engine::StyleManager* style_manager_ = nullptr;
    engine::PlatformInterface* platform_ = nullptr;
    engine::NxmIpcServer* nxm_ipc_ = nullptr;
    std::unique_ptr<engine::DeploymentStrategy> deploy_strategy_;
    bool nxm_handler_check_done_ = false;

    void save_app_state();
    void restore_app_state();
    void restore_exec_selection();
    QJsonObject read_app_state_extra() const;
    std::filesystem::path app_state_path() const;
    void save_download_manifest();
    void load_download_manifest();
    std::filesystem::path download_manifest_path() const;
    // Wire the Downloads tab for the current instance: load its manifest,
    // point it at the instance downloads dir (starts the watchdog), and
    // connect its install/pause/resume/removal signals. Called after every
    // right-panel rebuild (startup and instance switch), where the tab is
    // freshly created and would otherwise be inert.
    void wire_downloads_tab();

    std::string current_game_id_;
    std::string current_game_name_;
    std::string current_profile_name_ = "Default";
    std::filesystem::path current_game_dir_;
    std::filesystem::path current_instance_root_;
    bool loading_ = false;
    // Plugin database driving the Plugins tab (empty until a plugin-capable
    // game is loaded). Rebuilt on refresh; toggles/moves save the profile.
    engine::PluginDatabase plugins_db_;
    ui::PluginsTab* plugins_tab_widget_ = nullptr;
    // Long-lived LOOT sort worker thread (created on first use, reused).
    ui::LootSortThread* loot_sort_thread_ = nullptr;
    // Data tab context-menu targets. data_tab_widget_ is set by set_game_info()
    // after each right-panel rebuild; preview_window_ is lazily created on the
    // first preview request and kept across rebuilds.
    ui::DataTab* data_tab_widget_ = nullptr;
    ui::preview::PreviewWindow* preview_window_ = nullptr;
    // Selection-highlight indexes, rebuilt once per plugin refresh (O(P)); the
    // per-selection work is then lookups only, so huge mod lists stay cheap.
    // owner_mod -> plugin names the mod owns; name -> row in plugins_db_.
    QHash<QString, QVector<QString>> plugin_owner_index_;
    QHash<QString, int> plugin_row_by_name_;
    QStringList toolbar_shortcut_paths_;
    // Custom icon path for each toolbar shortcut, kept in lockstep with
    // toolbar_shortcut_paths_ (same index) and persisted to instance.toml so
    // custom icons survive restarts.
    QStringList toolbar_shortcut_icons_;
    std::vector<std::string> saved_executables_;
    std::string pending_nxm_url_;
    // In-flight/known Nexus downloads keyed by "<mod_id>-<file_id>", kept so a
    // paused download can be resumed with its original NXM link.
    std::unordered_map<std::string, engine::NxmLink> nxm_links_;
    // In-flight/known LoversLab downloads keyed by the download id, kept so a
    // paused download can be resumed with its original ?do=download URL.
    std::unordered_map<std::string, std::string> url_downloads_;
    int64_t running_process_pid_ = -1;
    QTimer* process_watch_timer_ = nullptr;
    bool overlay_launched_ = false;
    std::string cgroup_path_;  // cgroup v2 path for process tracking (empty = unavailable)
    std::filesystem::file_time_type launch_time_;
    std::filesystem::path staging_dir_;  // non-empty when OverlayFS deploy strategy is active
    // "Output to mod" session: scratch capture dir + target mod folder.
    // Both empty = default Overwrite capture.
    std::filesystem::path output_session_scratch_;
    std::filesystem::path output_mod_dir_;
    std::filesystem::path conflict_cache_path_;  // path to conflict cache JSON
    engine::PathRegistry last_conflict_registry_;
    // Conflict recompute machinery (P8.1): debounce timer coalesces rapid
    // toggle/reorder requests; the scan runs on ConflictScanThread with at most
    // one in flight (conflict_scan_running_); requests arriving mid-scan queue
    // a fresh scan (conflict_scan_pending_); generation drops stale results;
    // invalidations of the quick-token cache are applied by the worker before
    // it scans. Follow-ups run on the main thread after the results land.
    QTimer* conflict_debounce_timer_ = nullptr;
    ui::ConflictScanThread* conflict_scan_thread_ = nullptr;
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
    ui::ModScanThread* mod_scan_thread_ = nullptr;
    quint64 mod_scan_generation_ = 0;
    // Plugin-DB preload machinery (P8.5/T6): launch_plugin_db_preload() runs
    // the plugin-DB disk load concurrently with the mod scan on
    // PluginDbLoadThread (gmm-plugin-db). plugin_db_generation_ drops a stale
    // load (instance switch bumps it); preload_pending_ is true only between a
    // launch and its consumption — either adoption by refresh_plugins_tab() or
    // a synchronous fallback read (which discards the pending preload so it
    // can't land late and clobber fresher data).
    ui::PluginDbLoadThread* plugin_db_load_thread_ = nullptr;
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
    ui::DeployThread* launch_deploy_thread_ = nullptr;
    bool launch_prep_pending_ = false;
    engine::Instance current_instance_;  // loaded per-folder overrides for the active instance
    QPointer<ui::ModInfoDialog> modinfo_dialog_;  // alive while the dialog is open
    std::filesystem::path meta_dir_path() const;
    std::filesystem::path mods_dir_path() const;
    std::filesystem::path downloads_dir_path() const;
    std::filesystem::path cache_dir_path() const;
    std::filesystem::path cache_thumbnails_dir_path() const;
    std::filesystem::path profiles_dir_path() const;
    std::filesystem::path overwrite_dir_path() const;
    // Game's My Games folder under the prefix Documents dir (MO2's
    // documentsDirectory). Empty when the game has no prefix / appid.
    std::filesystem::path game_mygames_dir() const;
    std::filesystem::path resolve_mod_folder(const std::string& mod_id, const std::string& mods_subpath) const;
    QByteArray pending_geometry_;
    // Restored app state, applied once the widgets are ready
    int icon_size_ = 24;                 // toolbar icon size (small/medium/large)
    QString pending_exec_selection_;     // last selected executable path, per instance

    // Konami code easter egg
    DebugWindow* debug_window_ = nullptr;
    PipelineWindow* pipeline_window_ = nullptr;
    int konami_state_ = 0;
    static constexpr int konami_sequence_[10] = {
        Qt::Key_Up,    Qt::Key_Up,
        Qt::Key_Down,  Qt::Key_Down,
        Qt::Key_Left,  Qt::Key_Right,
        Qt::Key_Left,  Qt::Key_Right,
        Qt::Key_B,     Qt::Key_A
    };

    // Pending changes queue (deferred until game exits)
    std::vector<PendingToggle> pending_changes_;
    QLabel* pending_queue_label_ = nullptr;

    // Game-lock overlay
    QWidget* game_lock_overlay_ = nullptr;
    QLabel* game_lock_label_ = nullptr;
    QPushButton* unlock_button_ = nullptr;
    QPushButton* kill_button_ = nullptr;
    QCheckBox* process_tree_checkbox_ = nullptr;
    QTreeWidget* process_tree_ = nullptr;
    int64_t locked_pid_ = -1;
    bool show_process_tree_ = false;

    // Install-progress popup state. Lazily created on the first install;
    // kept across installs within the session. install_progress_show_timer_
    // defers the first show by ~300ms so a fast install never flashes the
    // dialog; active_install_progress_id_ tracks which install the dialog
    // belongs to so a new install resets it.
    ui::InstallProgressDialog* install_progress_dialog_ = nullptr;
    QTimer* install_progress_show_timer_ = nullptr;
    std::string active_install_progress_id_;
};

}  // namespace ui
