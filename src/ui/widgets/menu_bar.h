#pragma once

#include <QMenuBar>
#include <string>
#include <vector>

#include "engine/tools/external_tool.h"

namespace ui {

class MainWindow;

// Application menu bar - File / Edit / View / Tools / Help.
// Created once per MainWindow, lives at the top of the window.
// Actions emit signals; MainWindow connects them to actual behavior.
class AppMenuBar : public QMenuBar {
    Q_OBJECT
public:
    explicit AppMenuBar(MainWindow* parent);

    void set_recent_instances(const std::vector<std::string>& instances);
    void update_tools_for_game(const std::string& game_id,
                               const std::vector<engine::ExternalTool>& tools);
    void set_sort_available(bool available);

signals:
    // File
    void new_instance_requested();
    void open_instance_requested();
    void recent_instance_selected(const QString& name);
    void import_mods_requested();
    void export_mods_requested();
    void settings_requested();
    void exit_requested();

    // Edit
    void select_all_requested();
    void deselect_all_requested();
    void enable_selected_requested();
    void disable_selected_requested();
    void priority_up_requested();
    void priority_down_requested();

    // View
    void toggle_toolbar(bool visible);
    void toggle_status_bar(bool visible);
    void toggle_console(bool visible);
    void pipeline_requested();
    void icon_size_requested(int size);
    void refresh_requested();

    // Tools
    void tool_requested(const QString& tool_id, const QString& game_id);
    void sort_mods_requested();
    void open_instance_folder_requested();
    void open_mods_folder_requested();
    void open_downloads_folder_requested();

    // Help
    void about_requested();
    void about_qt_requested();
    void instance_statistics_requested();

private:
    void build_file_menu();
    void build_edit_menu();
    void build_view_menu();
    void build_tools_menu();
    void build_help_menu();

    QMenu* recent_menu_ = nullptr;
    QMenu* tools_menu_ = nullptr;
    QAction* tools_separator_ = nullptr;
    QAction* sort_action_ = nullptr;
    std::string current_game_id_;
};

}  // namespace ui
