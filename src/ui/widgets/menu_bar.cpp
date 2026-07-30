#include "ui/widgets/menu_bar.h"
#include "ui/main_window/main_window.h"

#include <QAction>
#include <QApplication>
#include <QDesktopServices>
#include <QMessageBox>
#include <QUrl>

namespace ui {

AppMenuBar::AppMenuBar(MainWindow* parent)
    : QMenuBar(parent) {
    build_file_menu();
    build_edit_menu();
    build_view_menu();
    build_tools_menu();
    build_help_menu();
}

// --------- File ---------

void AppMenuBar::build_file_menu() {
    auto* menu = addMenu("&File");

    auto* new_inst = menu->addAction("New Instance...");
    new_inst->setShortcut(QKeySequence::New);
    connect(new_inst, &QAction::triggered, this, &AppMenuBar::new_instance_requested);

    auto* open_inst = menu->addAction("Open Instance...");
    open_inst->setShortcut(QKeySequence::Open);
    connect(open_inst, &QAction::triggered, this, &AppMenuBar::open_instance_requested);

    recent_menu_ = menu->addMenu("Recent Instances");
    recent_menu_->setEnabled(false);  // disabled until populated

    menu->addSeparator();

    auto* import_mods = menu->addAction("Import Mods...");
    import_mods->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_I));
    connect(import_mods, &QAction::triggered, this, &AppMenuBar::import_mods_requested);

    auto* export_mods = menu->addAction("Export Mods...");
    export_mods->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E));
    connect(export_mods, &QAction::triggered, this, &AppMenuBar::export_mods_requested);

    menu->addSeparator();

    auto* settings = menu->addAction("Settings...");
    settings->setShortcut(QKeySequence::Preferences);
    connect(settings, &QAction::triggered, this, &AppMenuBar::settings_requested);

    menu->addSeparator();

    auto* exit = menu->addAction("Exit");
    exit->setShortcut(QKeySequence::Quit);
    connect(exit, &QAction::triggered, this, &AppMenuBar::exit_requested);
}

// --------- Edit ---------

void AppMenuBar::build_edit_menu() {
    auto* menu = addMenu("&Edit");

    auto* select_all = menu->addAction("Select All");
    select_all->setShortcut(QKeySequence::SelectAll);
    connect(select_all, &QAction::triggered, this, &AppMenuBar::select_all_requested);

    auto* deselect_all = menu->addAction("Deselect All");
    deselect_all->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A));
    connect(deselect_all, &QAction::triggered, this, &AppMenuBar::deselect_all_requested);

    menu->addSeparator();

    auto* enable = menu->addAction("Enable Selected");
    enable->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_E));
    connect(enable, &QAction::triggered, this, &AppMenuBar::enable_selected_requested);

    auto* disable = menu->addAction("Disable Selected");
    disable->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_D));
    connect(disable, &QAction::triggered, this, &AppMenuBar::disable_selected_requested);

    menu->addSeparator();

    auto* prio_up = menu->addAction("Priority Up");
    prio_up->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Up));
    connect(prio_up, &QAction::triggered, this, &AppMenuBar::priority_up_requested);

    auto* prio_down = menu->addAction("Priority Down");
    prio_down->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Down));
    connect(prio_down, &QAction::triggered, this, &AppMenuBar::priority_down_requested);
}

// --------- View ---------

void AppMenuBar::build_view_menu() {
    auto* menu = addMenu("&View");

    auto* toggle_toolbar = menu->addAction("Show Toolbar");
    toggle_toolbar->setCheckable(true);
    toggle_toolbar->setChecked(true);
    connect(toggle_toolbar, &QAction::toggled, this, &AppMenuBar::toggle_toolbar);

    auto* toggle_status = menu->addAction("Show Status Bar");
    toggle_status->setCheckable(true);
    toggle_status->setChecked(true);
    connect(toggle_status, &QAction::toggled, this, &AppMenuBar::toggle_status_bar);

    auto* toggle_console = menu->addAction("Show Console");
    toggle_console->setCheckable(true);
    toggle_console->setChecked(false);
    toggle_console->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Backtab));
    connect(toggle_console, &QAction::toggled, this, &AppMenuBar::toggle_console);

    menu->addSeparator();

    columns_menu_ = menu->addMenu("Columns");

    menu->addSeparator();

    auto* refresh = menu->addAction("Refresh");
    refresh->setShortcut(QKeySequence::Refresh);
    connect(refresh, &QAction::triggered, this, &AppMenuBar::refresh_requested);
}

// --------- Tools ---------

void AppMenuBar::build_tools_menu() {
    tools_menu_ = addMenu("&Tools");

    // Dynamic tools section — populated by update_tools_for_game()
    // (nothing here yet; added when a game is selected)

    tools_separator_ = tools_menu_->addSeparator();

    sort_action_ = tools_menu_->addAction("Sort Mods");
    sort_action_->setVisible(false);
    connect(sort_action_, &QAction::triggered, this, &AppMenuBar::sort_mods_requested);

    auto* open_inst = tools_menu_->addAction("Open Instance Folder");
    connect(open_inst, &QAction::triggered, this, &AppMenuBar::open_instance_folder_requested);

    auto* open_mods = tools_menu_->addAction("Open Mods Folder");
    connect(open_mods, &QAction::triggered, this, &AppMenuBar::open_mods_folder_requested);

    auto* open_dl = tools_menu_->addAction("Open Downloads Folder");
    connect(open_dl, &QAction::triggered, this, &AppMenuBar::open_downloads_folder_requested);
}

void AppMenuBar::update_tools_for_game(
    const std::string& game_id,
    const std::vector<engine::ExternalTool>& tools) {
    current_game_id_ = game_id;

    // Remove all actions before the separator (dynamic tools section)
    auto actions = tools_menu_->actions();
    for (auto* act : actions) {
        if (act == tools_separator_) break;
        tools_menu_->removeAction(act);
        delete act;
    }

    // Add registered tools for this game
    if (!tools.empty()) {
        for (const auto& tool : tools) {
            QString label = QString::fromStdString(
                tool.display_name.empty() ? tool.tool_id : tool.display_name);
            auto* act = tools_menu_->addAction(label);
            QString tid = QString::fromStdString(tool.tool_id);
            QString gid = QString::fromStdString(game_id);
            connect(act, &QAction::triggered, this, [this, tid, gid]() {
                emit tool_requested(tid, gid);
            });
        }
        tools_menu_->insertSeparator(tools_separator_);
    }
}

void AppMenuBar::set_sort_available(bool available) {
    if (sort_action_) sort_action_->setVisible(available);
}

// --------- Help ---------

void AppMenuBar::build_help_menu() {
    auto* menu = addMenu("&Help");

    auto* stats = menu->addAction("Instance Statistics...");
    connect(stats, &QAction::triggered, this, &AppMenuBar::instance_statistics_requested);

    menu->addSeparator();

    auto* about = menu->addAction("About GameModManager");
    connect(about, &QAction::triggered, this, &AppMenuBar::about_requested);

    auto* about_qt = menu->addAction("About Qt");
    connect(about_qt, &QAction::triggered, this, &AppMenuBar::about_qt_requested);

    menu->addSeparator();

    auto* updates = menu->addAction("Check for Updates...");
    connect(updates, &QAction::triggered, this, &AppMenuBar::check_updates_requested);
}

// --------- Recent instances ---------

void AppMenuBar::set_recent_instances(const std::vector<std::string>& instances) {
    recent_menu_->clear();
    if (instances.empty()) {
        recent_menu_->setEnabled(false);
        return;
    }
    recent_menu_->setEnabled(true);
    for (const auto& name : instances) {
        auto* action = recent_menu_->addAction(QString::fromStdString(name));
        connect(action, &QAction::triggered, this, [this, name]() {
            emit recent_instance_selected(QString::fromStdString(name));
        });
    }
}

}  // namespace ui
