#pragma once

#include <QMainWindow>
#include <QByteArray>
#include <QStringList>
#include <filesystem>
#include <string>

class QSplitter;
class QToolBar;
class QMenu;

namespace engine {
class GameKnowledge;
}

namespace ui {

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

    [[nodiscard]] ModTableView* mod_view() const { return mod_view_; }
    [[nodiscard]] QSplitter* console_splitter() const { return console_splitter_; }

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void on_notification(const QString& title, const QString& message);

private:
    void update_title();
    void setup_menu_bar();
    void connect_menu_actions();
    void setup_mod_list_context_menu();
    void load_mods_from_game();
    void sync_mod_enable_state(const QString& mod_id, bool enabled);
    void sync_priorities();
    void create_separator();
    void edit_separator(int row);
    void delete_separator(int row);
    void save_order();
    void load_order();
    void populate_executables();
    void launch_game();
    void launch_with_executable(const QString& full_path);
    void add_shortcut_to_toolbar();
    void add_toolbar_shortcut_from_path(const QString& full_path);
    void add_shortcut_to_desktop();

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
    QMenu* mod_context_menu_ = nullptr;
    engine::GameKnowledge* knowledge_ = nullptr;

    void save_app_state();
    void restore_app_state();
    std::filesystem::path app_state_path() const;

    std::string current_game_id_;
    std::string current_game_name_;
    std::string current_profile_name_ = "Default";
    std::filesystem::path current_game_dir_;
    std::filesystem::path current_instance_root_;
    bool loading_ = false;
    QStringList toolbar_shortcut_paths_;
};

}  // namespace ui
