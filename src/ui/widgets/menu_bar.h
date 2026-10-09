#pragma once

#include <QActionGroup>
#include <QMenu>
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
  explicit AppMenuBar(MainWindow *parent);

  void set_recent_instances(const std::vector<std::string> &instances);
  void update_tools_for_game(const std::string &game_id,
                             const std::vector<engine::ExternalTool> &tools);
  void set_sort_available(bool available);
  void set_icon_size(int size);

  // View menu checkbox sync - update checked state without re-emitting toggled
  void set_toolbar_checked(bool checked);
  void set_status_bar_checked(bool checked);
  void set_console_checked(bool checked);
  // The menu bar's own checkbox, synced from MainWindow::keyReleaseEvent when
  // Alt brings a hidden bar back - the toggle cannot stay unchecked while the
  // bar is on screen.
  void set_menu_bar_checked(bool checked);
  // Sync the Checkerboard submenu checked state (0=off, 1=light, 2=medium,
  // 3=dark) without re-emitting checkerboard_style_requested.
  void set_checkerboard_style(int style);

  // The managed game's own support wiki, offered in the Help menu only while
  // it is non-empty (MO2 gates the entry the same way). Passing an empty
  // string takes the entry back out of the menu.
  void set_game_support_url(const QString &url);

  // The Help > Tutorials entries, as (title, address) pairs. The submenu is
  // offered only while the list is non-empty; passing an empty list takes it
  // back out of the menu.
  void set_tutorials(const QList<QPair<QString, QString>> &tutorials);

signals:
  // File
  void new_instance_requested();
  void open_instance_requested();
  void recent_instance_selected(const QString &name);
  void import_mods_requested();
  void export_mods_requested();
  void import_modpack_requested();
  void export_modpack_requested();
  void mod_source_page_requested();
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
  // MO2 actionMainMenuToggle (mainwindow.ui:menuToolbars) - the third
  // visibility toggle in that menu, and the only one of the three missing.
  void toggle_menu_bar(bool visible);
  void pipeline_requested();
  void icon_size_requested(int size);
  // MO2 actionToolBarIconsOnly / TextOnly / IconsAndText: how toolbar buttons
  // render, as a Qt::ToolButtonStyle value.
  void tool_button_style_requested(int style);
  void checkerboard_style_requested(int style);
  void refresh_requested();

  // Tools
  void tool_requested(const QString &tool_id, const QString &game_id);
  void sort_mods_requested();
  void tool_plugins_requested();
  void profiles_requested();
  void executables_requested();

  // Help
  void help_on_ui_requested();
  void open_url_requested(const QString &url);
  void about_requested();
  void about_qt_requested();
  void instance_statistics_requested();
  void debug_panel_requested();

private:
  void build_file_menu();
  void build_edit_menu();
  void build_view_menu();
  void build_tools_menu();
  void build_help_menu();

  QMenu *recent_menu_       = nullptr;
  QMenu *tools_menu_        = nullptr;
  QMenu *icons_menu_        = nullptr;
  // Help > Tutorials. Hidden until set_tutorials() has entries to put in it.
  QMenu *tutorials_menu_     = nullptr;
  QAction *tools_separator_ = nullptr;
  QAction *sort_action_     = nullptr;
  // Help > Game Support Wiki. Hidden until set_game_support_url() has a URL.
  QAction *game_support_wiki_action_ = nullptr;
  QString game_support_url_;
  // View menu actions - stored so their checked state can be synced
  // with actual panel visibility from any code path
  QAction *toggle_toolbar_action_    = nullptr;
  QAction *toggle_status_bar_action_ = nullptr;
  QAction *toggle_console_action_    = nullptr;
  QAction *toggle_menu_bar_action_   = nullptr;
  // Toolbar button style group (Icons Only / Text Only / Icons and Text) for
  // sync; the chosen style is also what the group reports back.
  QActionGroup *tool_button_style_group_ = nullptr;
  // Checkerboard submenu actions (Off/Light/Medium/Dark) for sync.
  QActionGroup *checkerboard_group_ = nullptr;
  std::string current_game_id_;
};

}  // namespace ui
