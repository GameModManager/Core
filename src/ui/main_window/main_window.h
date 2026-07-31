#pragma once

#include <QMainWindow>
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "engine/index/conflict_engine.h"
#include "engine/meta/mod_meta.h"
#include "engine/deploy/strategy.h"

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
    void edit_separator(int row);
    void delete_separator(int row);
    void save_order();
    void load_order();
    void save_executables();
    void load_executables();
    void sync_separator_ids();
    void group_mods_by_separator();
    void populate_executables();
    void launch_game();
    void launch_with_executable(const QString& full_path);
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
    void recompute_conflicts();
    void on_image_diff_requested(const QString& relative_path);
    void migrate_mo2_meta();
    void load_meta_for_mods();
    void show_instance_statistics();
    void show_settings_dialog();
    void show_pipeline_window();

    // Context menu helpers
    void clear_overwrite();
    void create_mod_from_overwrite();
    void remove_selected_mods();
    void move_to_separator(const QString& mod_id, const QString& sep_id);
    void send_to_highest_priority(const QString& id);
    void send_to_lowest_priority(const QString& id);
    void send_to_highest_in_separator(const QString& id);
    void send_to_lowest_in_separator(const QString& id);
    void priority_move_selected(int step);
    void toggle_selected_mods(bool enabled);
    void rename_selected_mod();
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
    engine::StyleManager* style_manager_ = nullptr;
    engine::NxmIpcServer* nxm_ipc_ = nullptr;
    std::unique_ptr<engine::DeploymentStrategy> deploy_strategy_;

    void save_app_state();
    void restore_app_state();
    std::filesystem::path app_state_path() const;
    void save_download_manifest();
    void load_download_manifest();
    std::filesystem::path download_manifest_path() const;

    std::string current_game_id_;
    std::string current_game_name_;
    std::string current_profile_name_ = "Default";
    std::filesystem::path current_game_dir_;
    std::filesystem::path current_instance_root_;
    bool loading_ = false;
    QStringList toolbar_shortcut_paths_;
    std::vector<std::string> saved_executables_;
    std::string pending_nxm_url_;
    int64_t running_process_pid_ = -1;
    QTimer* process_watch_timer_ = nullptr;
    bool overlay_launched_ = false;
    std::string cgroup_path_;  // cgroup v2 path for process tracking (empty = unavailable)
    std::filesystem::file_time_type launch_time_;
    std::filesystem::path staging_dir_;  // non-empty when OverlayFS deploy strategy is active
    std::filesystem::path conflict_cache_path_;  // path to conflict cache JSON
    engine::PathRegistry last_conflict_registry_;
    std::filesystem::path meta_dir_path() const;
    std::filesystem::path mods_dir_path() const;
    std::filesystem::path resolve_mod_folder(const std::string& mod_id, const std::string& mods_subpath) const;
    QByteArray pending_geometry_;

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
