#pragma once

#include <QMainWindow>
#include <string>

class QSplitter;
class QToolBar;
class QTableView;

namespace ui {

class ModListModel;
class MainToolbar;
class ProfileBar;
class ModFilterBar;
class RightPanel;
class ConsolePanel;
class GmmStatusBar;
class PipelineThread;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

    void set_game_info(const std::string& game_id,
                       const std::string& game_display_name,
                       const std::string& profile_name = "Default");

private slots:
    void on_notification(const QString& title, const QString& message);

private:
    void update_title();

    QToolBar* toolbar_area_ = nullptr;
    MainToolbar* toolbar_ = nullptr;
    ProfileBar* profile_bar_ = nullptr;
    ModFilterBar* filter_bar_ = nullptr;
    QTableView* mod_view_ = nullptr;
    ModListModel* mod_model_ = nullptr;
    RightPanel* right_panel_ = nullptr;
    QSplitter* main_splitter_ = nullptr;
    QSplitter* console_splitter_ = nullptr;
    ConsolePanel* console_ = nullptr;
    GmmStatusBar* status_bar_ = nullptr;
    PipelineThread* pipeline_thread_ = nullptr;

    std::string current_game_id_;
    std::string current_game_name_;
    std::string current_profile_name_ = "Default";
};

}  // namespace ui
