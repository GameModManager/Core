#pragma once

#include <QMainWindow>
#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>
#include <filesystem>
#include <memory>
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

class QSplitter;
class QToolBar;
class QMenu;
class QKeyEvent;
class QLabel;
class QPushButton;
class QCheckBox;
class QTreeWidget;

namespace engine {
class GameKnowledge;
class PluginLoader;
class ManagedGames;
class NxmIpcServer;
struct NxmLink;
struct ConflictStats;
class StyleManager;
class PlatformInterface;
}

namespace ui {

class DebugWindow;
class ModListModel;
class ModTableView;
class MainToolbar;
class ProfileBar;
class ModFilterBar;
class RightPanel;
class ConsolePanel;
class GmmStatusBar;
class PipelineThread;
class AppMenuBar;
class PipelineWindow;
class PluginsTab;
class DataTab;
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

    [[nodiscard]] ModTableView* mod_view() const { return mod_view_; }
    [[nodiscard]] QSplitter* console_splitter() const { return console_splitter_; }

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
    void apply_mod_filter();
    void do_capture_overwrite(std::filesystem::file_time_type capture_time);
    void flush_pending_nxm();
    void flush_pending_changes();
    void update_queue_label();
    void prompt_nxm_registration();
    void ensure_nxm_handler_default();
    void recompute_conflicts();
    void refresh_data_tab();
    void wire_data_tab();
    void on_data_open(const QString& file_path);
    void on_data_execute(const QString& file_path, bool is_windows_exe);
    void on_data_preview(const QString& file_path,
                         const QStringList& provider_paths,
                         const QStringList& provider_names);
    void on_data_add_executable(const QString& file_path, const QString& default_name);
    void on_data_mod_info(const QString& mod_id);
    void on_data_hide(const QString& file_path, const QString& mod_id, bool hide);
    void on_image_diff_requested(const QString& relative_path);
    void migrate_mo2_meta();
    void load_meta_for_mods();
    void show_instance_statistics();
    void show_settings_dialog();
    void show_pipeline_window();

    // Plugins tab (Skyrim-style games with plugin support).
    void refresh_plugins_tab();
    void on_plugin_toggle(const std::string& name, bool enabled);
    void on_plugin_reorder(int from_row, int to_row);
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
    void open_source_for_mod(const QString& source_type, const QString& source_id);
    [[nodiscard]] SourceVisitInfo source_visit_info(const QString& source_type, const QString& source_id) const;

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
    std::vector<std::string> saved_executables_;
    std::string pending_nxm_url_;
    // In-flight/known Nexus downloads keyed by "<mod_id>-<file_id>", kept so a
    // paused download can be resumed with its original NXM link.
    std::unordered_map<std::string, engine::NxmLink> nxm_links_;
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
    engine::Instance current_instance_;  // loaded per-folder overrides for the active instance
    std::filesystem::path meta_dir_path() const;
    std::filesystem::path mods_dir_path() const;
    std::filesystem::path downloads_dir_path() const;
    std::filesystem::path cache_dir_path() const;
    std::filesystem::path cache_thumbnails_dir_path() const;
    std::filesystem::path profiles_dir_path() const;
    std::filesystem::path overwrite_dir_path() const;
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
};

}  // namespace ui
