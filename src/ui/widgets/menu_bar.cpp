#include "ui/widgets/menu_bar.h"
#include "ui/main_window/main_window.h"
#include "ui/preview/preview_widget.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QDesktopServices>
#include <QMessageBox>
#include <QUrl>

#include <utility>

namespace ui {

AppMenuBar::AppMenuBar(MainWindow *parent) : QMenuBar(parent) {
  setWhatsThis(tr("The application's menus. \"Help > Help on UI\" turns on a pointer "
                  "you can then click any element with to read what it does."));
  build_file_menu();
  build_edit_menu();
  build_view_menu();
  build_tools_menu();
  build_help_menu();
}

// --------- File ---------

void AppMenuBar::build_file_menu() {
  auto *menu = addMenu(tr("&File"));

  auto *new_inst = menu->addAction(tr("New Instance..."));
  connect(new_inst, &QAction::triggered, this, &AppMenuBar::new_instance_requested);

  auto *open_inst = menu->addAction(tr("Open Instance..."));
  open_inst->setShortcut(QKeySequence::Open);
  connect(open_inst, &QAction::triggered, this, &AppMenuBar::open_instance_requested);

  recent_menu_ = menu->addMenu(tr("Recent Instances"));
  recent_menu_->setEnabled(false);  // disabled until populated

  menu->addSeparator();

  auto *import_mods = menu->addAction(tr("Import Mods..."));
  // MO2's Ctrl+M Install Mod installs a new mod from an archive, which is what
  // this entry does; Ctrl+Shift+I stays alongside it.
  import_mods->setShortcuts({QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_I),
                             QKeySequence(Qt::CTRL | Qt::Key_M)});
  connect(import_mods, &QAction::triggered, this, &AppMenuBar::import_mods_requested);

  auto *export_mods = menu->addAction(tr("Export Mods..."));
  export_mods->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E));
  connect(export_mods, &QAction::triggered, this, &AppMenuBar::export_mods_requested);

  auto *import_modpack = menu->addAction(tr("Import Modpack..."));
  connect(import_modpack, &QAction::triggered, this,
          &AppMenuBar::import_modpack_requested);

  auto *export_modpack = menu->addAction(tr("Export Modpack..."));
  export_modpack->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_M));
  connect(export_modpack, &QAction::triggered, this,
          &AppMenuBar::export_modpack_requested);

  // MO2's Ctrl+N opens the selected mod's page on the site it came from, under
  // the generic name rather than a vendor's brand, because the source may be
  // Nexus, the Workshop, LoversLab, ModDB or mod.pub. Stays enabled with
  // nothing selected - the handler is a no-op there, same as MO2's.
  auto *mod_source = menu->addAction(tr("Open Mod Source Page"));
  mod_source->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_N));
  connect(mod_source, &QAction::triggered, this, &AppMenuBar::mod_source_page_requested);

  menu->addSeparator();

  auto *settings = menu->addAction(tr("Settings..."));
  // MO2 binds Settings to Ctrl+S; the platform-native Preferences key stays.
  settings->setShortcuts(
      {QKeySequence::Preferences, QKeySequence(Qt::CTRL | Qt::Key_S)});
  connect(settings, &QAction::triggered, this, &AppMenuBar::settings_requested);

  menu->addSeparator();

  auto *exit = menu->addAction(tr("Exit"));
  exit->setShortcut(QKeySequence::Quit);
  connect(exit, &QAction::triggered, this, &AppMenuBar::exit_requested);
}

// --------- Edit ---------

void AppMenuBar::build_edit_menu() {
  auto *menu = addMenu(tr("&Edit"));

  auto *select_all = menu->addAction(tr("Select All"));
  select_all->setShortcut(QKeySequence::SelectAll);
  connect(select_all, &QAction::triggered, this, &AppMenuBar::select_all_requested);

  auto *deselect_all = menu->addAction(tr("Deselect All"));
  deselect_all->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A));
  connect(deselect_all, &QAction::triggered, this, &AppMenuBar::deselect_all_requested);

  menu->addSeparator();

  // Ctrl+E is MO2's Modify Executables, over in Tools; the pair of checkbox
  // entries carries no shortcut of its own.
  auto *enable = menu->addAction(tr("Enable Selected"));
  connect(enable, &QAction::triggered, this, &AppMenuBar::enable_selected_requested);

  auto *disable = menu->addAction(tr("Disable Selected"));
  disable->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_D));
  connect(disable, &QAction::triggered, this, &AppMenuBar::disable_selected_requested);

  menu->addSeparator();

  auto *prio_up = menu->addAction(tr("Priority Up"));
  prio_up->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Up));
  connect(prio_up, &QAction::triggered, this, &AppMenuBar::priority_up_requested);

  auto *prio_down = menu->addAction(tr("Priority Down"));
  prio_down->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Down));
  connect(prio_down, &QAction::triggered, this, &AppMenuBar::priority_down_requested);
}

// --------- View ---------

void AppMenuBar::build_view_menu() {
  auto *menu = addMenu(tr("&View"));

  toggle_toolbar_action_ = menu->addAction(tr("Show Toolbar"));
  toggle_toolbar_action_->setCheckable(true);
  toggle_toolbar_action_->setChecked(true);
  connect(toggle_toolbar_action_, &QAction::toggled, this, &AppMenuBar::toggle_toolbar);

  toggle_status_bar_action_ = menu->addAction(tr("Show Status Bar"));
  toggle_status_bar_action_->setCheckable(true);
  toggle_status_bar_action_->setChecked(true);
  connect(toggle_status_bar_action_, &QAction::toggled, this,
          &AppMenuBar::toggle_status_bar);

  toggle_console_action_ = menu->addAction(tr("Show Console"));
  toggle_console_action_->setCheckable(true);
  toggle_console_action_->setChecked(false);
  toggle_console_action_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Backtab));
  connect(toggle_console_action_, &QAction::toggled, this, &AppMenuBar::toggle_console);

  // MO2 actionMainMenuToggle, the first entry of menuToolbars
  // (mainwindow.ui:1560): hide the whole menu bar. An Alt key release brings
  // it back (MO2 MainWindow::keyReleaseEvent), so this is a focus-saving
  // toggle rather than a permanent change.
  toggle_menu_bar_action_ = menu->addAction(tr("Show Menu Bar"));
  toggle_menu_bar_action_->setCheckable(true);
  toggle_menu_bar_action_->setChecked(true);
  connect(toggle_menu_bar_action_, &QAction::toggled, this,
          &AppMenuBar::toggle_menu_bar);

  menu->addSeparator();

  auto *pipeline = menu->addAction("Workflow Pipeline...");
  connect(pipeline, &QAction::triggered, this, &AppMenuBar::pipeline_requested);

  menu->addSeparator();

  auto *icons_menu  = menu->addMenu(tr("Icons"));
  icons_menu_       = icons_menu;
  auto *icons_group = new QActionGroup(this);
  icons_group->setExclusive(true);
  auto add_icon_size = [&](const QString &label, int size, bool checked) {
    auto *act = icons_menu->addAction(label);
    act->setCheckable(true);
    act->setData(size);
    act->setChecked(checked);
    icons_group->addAction(act);
    connect(act, &QAction::triggered, this, [this, size]() {
      emit icon_size_requested(size);
    });
  };
  add_icon_size(tr("Small"), 24, true);
  add_icon_size(tr("Medium"), 32, false);
  add_icon_size(tr("Large"), 48, false);

  // MO2's last three entries in menuToolbars (mainwindow.ui:1566-1570): how
  // the toolbar buttons render. One exclusive group, because these are three
  // renderings of the same toolbar rather than three independent switches.
  auto *style_menu         = menu->addMenu(tr("Toolbar Buttons"));
  tool_button_style_group_ = new QActionGroup(this);
  tool_button_style_group_->setExclusive(true);
  const std::pair<const char *, Qt::ToolButtonStyle> button_styles[] = {
      {"Icons Only", Qt::ToolButtonIconOnly},
      {"Text Only", Qt::ToolButtonTextOnly},
      {"Icons and Text", Qt::ToolButtonTextUnderIcon},
  };
  for (const auto &[label, style] : button_styles) {
    auto *act = style_menu->addAction(tr(label));
    act->setCheckable(true);
    act->setData(static_cast<int>(style));
    act->setChecked(style == Qt::ToolButtonIconOnly);
    tool_button_style_group_->addAction(act);
    connect(act, &QAction::triggered, this, [this, style]() {
      emit tool_button_style_requested(static_cast<int>(style));
    });
  }

  // Transparency checkerboard for image previews (PreviewWindow, Mod Info
  // images). Each entry shows a 2x2 swatch of the grid it selects.
  auto *checker_menu  = menu->addMenu(tr("Checkerboard"));
  checkerboard_group_ = new QActionGroup(this);
  checkerboard_group_->setExclusive(true);
  const std::pair<const char *, int> checker_styles[] = {
      {"Off", 0},
      {"Light", 1},
      {"Medium", 2},
      {"Dark", 3},
  };
  for (const auto &[label, style] : checker_styles) {
    auto *act = checker_menu->addAction(preview::checkerboard_icon(style), tr(label));
    act->setCheckable(true);
    act->setData(style);
    act->setChecked(style == 2);
    checkerboard_group_->addAction(act);
    connect(act, &QAction::triggered, this, [this, style]() {
      emit checkerboard_style_requested(style);
    });
  }

  menu->addSeparator();

  auto *refresh = menu->addAction(tr("Refresh"));
  refresh->setShortcut(QKeySequence::Refresh);
  connect(refresh, &QAction::triggered, this, &AppMenuBar::refresh_requested);
}

// --------- Tools ---------

void AppMenuBar::build_tools_menu() {
  tools_menu_ = addMenu(tr("&Tools"));

  // Dynamic tools section - populated by update_tools_for_game()
  // (nothing here yet; added when a game is selected)

  tools_separator_ = tools_menu_->addSeparator();

  sort_action_ = tools_menu_->addAction(tr("Sort Mods"));
  sort_action_->setVisible(false);
  connect(sort_action_, &QAction::triggered, this, &AppMenuBar::sort_mods_requested);

  // MO2's Tool Plugins entry (Ctrl+I) shows the loaded plugins; here that list
  // is the right panel's Plugins tab. Below the per-game tools because it is
  // an app-level view, not one of them.
  tools_menu_->addSeparator();
  auto *tool_plugins = tools_menu_->addAction(tr("Tool Plugins"));
  tool_plugins->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_I));
  connect(tool_plugins, &QAction::triggered, this, &AppMenuBar::tool_plugins_requested);

  // MO2 opens these two first in its Tools menu, ahead of the tool list. They
  // sit below the per-game tools here because update_tools_for_game() owns the
  // block above the first separator and rewrites it on every game switch.
  tools_menu_->addSeparator();
  auto *profiles = tools_menu_->addAction(tr("Profiles..."));
  profiles->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_P));
  connect(profiles, &QAction::triggered, this, &AppMenuBar::profiles_requested);

  auto *executables = tools_menu_->addAction(tr("Executables..."));
  executables->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_E));
  connect(executables, &QAction::triggered, this, &AppMenuBar::executables_requested);
}

void AppMenuBar::update_tools_for_game(const std::string &game_id,
                                       const std::vector<engine::ExternalTool> &tools) {
  current_game_id_ = game_id;

  // Remove all actions before the separator (dynamic tools section)
  auto actions = tools_menu_->actions();
  for (auto *act : actions) {
    if (act == tools_separator_)
      break;
    tools_menu_->removeAction(act);
    delete act;
  }

  // Add registered tools for this game (insert before the separator so the
  // clearing loop above reaches them -addAction appends to the end, past the
  // separator, which caused duplicates to accumulate on every switch).
  if (!tools.empty()) {
    for (const auto &tool : tools) {
      QString label = QString::fromStdString(
          tool.display_name.empty() ? tool.tool_id : tool.display_name);
      auto *act = new QAction(label, tools_menu_);
      tools_menu_->insertAction(tools_separator_, act);
      QString tid = QString::fromStdString(tool.tool_id);
      QString gid = QString::fromStdString(game_id);
      connect(act, &QAction::triggered, this, [this, tid, gid]() {
        emit tool_requested(tid, gid);
      });
    }
  }
}

void AppMenuBar::set_sort_available(bool available) {
  if (sort_action_)
    sort_action_->setVisible(available);
}

void AppMenuBar::set_icon_size(int size) {
  if (!icons_menu_)
    return;
  for (auto *act : icons_menu_->actions()) {
    if (act->data().toInt() == size) {
      act->setChecked(true);
      return;
    }
  }
}

// --------- View checkbox sync ---------

// MO2 MainWindow::createPopupMenu (mainwindow.cpp:821-838) puts the whole
// menuToolbars action set up front, then a separator, then View Log. Ours is
// the same shape with our own split: the three chrome toggles MO2 lists
// (toolbar, status bar, menu bar), then the separator, then the console -
// which is our View Log, and which MO2 reaches through the same menu.
QMenu *AppMenuBar::create_popup_menu(QWidget *parent) const {
  auto *menu = new QMenu(parent);
  if (toggle_toolbar_action_)
    menu->addAction(toggle_toolbar_action_);
  if (toggle_status_bar_action_)
    menu->addAction(toggle_status_bar_action_);
  if (toggle_menu_bar_action_)
    menu->addAction(toggle_menu_bar_action_);
  menu->addSeparator();
  if (toggle_console_action_)
    menu->addAction(toggle_console_action_);
  return menu;
}

void AppMenuBar::set_toolbar_checked(bool checked) {
  if (toggle_toolbar_action_) {
    toggle_toolbar_action_->blockSignals(true);
    toggle_toolbar_action_->setChecked(checked);
    toggle_toolbar_action_->blockSignals(false);
  }
}

void AppMenuBar::set_status_bar_checked(bool checked) {
  if (toggle_status_bar_action_) {
    toggle_status_bar_action_->blockSignals(true);
    toggle_status_bar_action_->setChecked(checked);
    toggle_status_bar_action_->blockSignals(false);
  }
}

void AppMenuBar::set_console_checked(bool checked) {
  if (toggle_console_action_) {
    toggle_console_action_->blockSignals(true);
    toggle_console_action_->setChecked(checked);
    toggle_console_action_->blockSignals(false);
  }
}

void AppMenuBar::set_menu_bar_checked(bool checked) {
  if (toggle_menu_bar_action_) {
    toggle_menu_bar_action_->blockSignals(true);
    toggle_menu_bar_action_->setChecked(checked);
    toggle_menu_bar_action_->blockSignals(false);
  }
}

void AppMenuBar::set_checkerboard_style(int style) {
  if (!checkerboard_group_)
    return;
  for (auto *act : checkerboard_group_->actions()) {
    if (act->data().toInt() == style) {
      // No blockSignals: the actions emit checkerboard_style_requested via
      // triggered(), which setChecked() never fires (only toggled()). Blocking
      // signals here would also defeat the exclusive QActionGroup, leaving the
      // previously-checked action checked alongside the new one.
      act->setChecked(true);
      return;
    }
  }
}

// --------- Help ---------

// Where the Help menu's link entries go. Both addresses are this project's
// own; a link entry with an empty URL keeps its place in the menu but is
// disabled, so the menu has MO2's shape without pointing at a missing page.
namespace {
constexpr auto kDocumentationUrl = "https://github.com/GameModManager/Core";
constexpr auto kIssueUrl         = "https://github.com/GameModManager/Core/issues";
}  // namespace

void AppMenuBar::build_help_menu() {
  auto *menu = addMenu(tr("&Help"));
  // MO2 binds Ctrl+H on the Help action itself, so the key pops the menu open
  // rather than firing one of its entries. QMenu has no setShortcut in Qt 6 -
  // the key rides on the menu's QAction, which is what QMenuBar watches.
  menu->menuAction()->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_H));

  auto link_entry = [this](QMenu *into, const QString &title, const QString &url) {
    auto *act = into->addAction(title);
    act->setData(url);
    act->setEnabled(!url.isEmpty());
    connect(act, &QAction::triggered, this,
            [this, url]() { emit open_url_requested(url); });
    return act;
  };

  auto *help_on_ui = menu->addAction(tr("Help on UI"));
  connect(help_on_ui, &QAction::triggered, this, &AppMenuBar::help_on_ui_requested);

  // The project's own addresses and the two About dialogs fold one level
  // down. MO2 keeps all four on the menu bar itself, at the same level as
  // Help on UI (mainwindow.cpp:1097-1162); More is where they live here.
  auto *more = menu->addMenu(tr("More"));
  link_entry(more, tr("Documentation"), kDocumentationUrl);
  link_entry(more, tr("Report Issue"), kIssueUrl);

  auto *about = more->addAction(tr("About GameModManager"));
  connect(about, &QAction::triggered, this, &AppMenuBar::about_requested);

  auto *about_qt = more->addAction(tr("About Qt"));
  connect(about_qt, &QAction::triggered, this, &AppMenuBar::about_qt_requested);

  // The game's own support wiki. The URL is read when the entry fires, not
  // when it is built, so a game switch that arrives later still opens the new
  // game's page.
  game_support_wiki_action_ = menu->addAction(tr("Game Support Wiki"));
  connect(game_support_wiki_action_, &QAction::triggered, this,
          [this]() { emit open_url_requested(game_support_url_); });
  set_game_support_url(game_support_url_);

  // MO2 fills this from the tutorials/*.js "//TL" headers, ordered by their
  // order value (mainwindow.cpp:1119-1160). Its submenu is live because MO2
  // ships the files it lists. Nothing here ships tutorial content, so the
  // submenu is held out of the menu until set_tutorials() has entries to put
  // in it: a submenu that opens onto nothing advertises something this build
  // cannot deliver, and a user who clicks it is left with no way forward.
  // Gated the same way Game Support Wiki is, on the content rather than on a
  // hardcoded disable, so it takes care of itself when content arrives.
  tutorials_menu_ = menu->addMenu(tr("Tutorials"));
  tutorials_menu_->menuAction()->setVisible(false);

  // GMM-only entries, below the help entries rather than mixed into them.
  menu->addSeparator();

  auto *stats = menu->addAction(tr("Instance Statistics..."));
  connect(stats, &QAction::triggered, this, &AppMenuBar::instance_statistics_requested);

  auto *debug = menu->addAction(tr("Debug Panel"));
  debug->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_D));
  connect(debug, &QAction::triggered, this, &AppMenuBar::debug_panel_requested);
}

void AppMenuBar::set_game_support_url(const QString &url) {
  game_support_url_ = url;
  if (!game_support_wiki_action_)
    return;
  game_support_wiki_action_->setVisible(!url.isEmpty());
  game_support_wiki_action_->setEnabled(!url.isEmpty());
}

void AppMenuBar::set_tutorials(const QList<QPair<QString, QString>> &tutorials) {
  if (!tutorials_menu_)
    return;
  tutorials_menu_->clear();
  for (const auto &[title, address] : tutorials) {
    auto *act = tutorials_menu_->addAction(title);
    act->setData(address);
    connect(act, &QAction::triggered, this,
            [this, address]() { emit open_url_requested(address); });
  }
  tutorials_menu_->menuAction()->setVisible(!tutorials.isEmpty());
}

// --------- Recent instances ---------

void AppMenuBar::set_recent_instances(const std::vector<std::string> &instances) {
  recent_menu_->clear();
  if (instances.empty()) {
    recent_menu_->setEnabled(false);
    return;
  }
  recent_menu_->setEnabled(true);
  for (const auto &name : instances) {
    auto *action = recent_menu_->addAction(QString::fromStdString(name));
    connect(action, &QAction::triggered, this, [this, name]() {
      emit recent_instance_selected(QString::fromStdString(name));
    });
  }
}

}  // namespace ui
