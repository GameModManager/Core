#include "ui/widgets/menu_bar.h"
#include "ui/main_window/main_window.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QDesktopServices>
#include <QMessageBox>
#include <QUrl>

namespace ui {

AppMenuBar::AppMenuBar(MainWindow *parent) : QMenuBar(parent) {
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
  new_inst->setShortcut(QKeySequence::New);
  connect(new_inst, &QAction::triggered, this,
          &AppMenuBar::new_instance_requested);

  auto *open_inst = menu->addAction(tr("Open Instance..."));
  open_inst->setShortcut(QKeySequence::Open);
  connect(open_inst, &QAction::triggered, this,
          &AppMenuBar::open_instance_requested);

  recent_menu_ = menu->addMenu(tr("Recent Instances"));
  recent_menu_->setEnabled(false); // disabled until populated

  menu->addSeparator();

  auto *import_mods = menu->addAction(tr("Import Mods..."));
  import_mods->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_I));
  connect(import_mods, &QAction::triggered, this,
          &AppMenuBar::import_mods_requested);

  auto *export_mods = menu->addAction(tr("Export Mods..."));
  export_mods->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E));
  connect(export_mods, &QAction::triggered, this,
          &AppMenuBar::export_mods_requested);

  menu->addSeparator();

  auto *settings = menu->addAction(tr("Settings..."));
  settings->setShortcut(QKeySequence::Preferences);
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
  connect(select_all, &QAction::triggered, this,
          &AppMenuBar::select_all_requested);

  auto *deselect_all = menu->addAction(tr("Deselect All"));
  deselect_all->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A));
  connect(deselect_all, &QAction::triggered, this,
          &AppMenuBar::deselect_all_requested);

  menu->addSeparator();

  auto *enable = menu->addAction(tr("Enable Selected"));
  enable->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_E));
  connect(enable, &QAction::triggered, this,
          &AppMenuBar::enable_selected_requested);

  auto *disable = menu->addAction(tr("Disable Selected"));
  disable->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_D));
  connect(disable, &QAction::triggered, this,
          &AppMenuBar::disable_selected_requested);

  menu->addSeparator();

  auto *prio_up = menu->addAction(tr("Priority Up"));
  prio_up->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Up));
  connect(prio_up, &QAction::triggered, this,
          &AppMenuBar::priority_up_requested);

  auto *prio_down = menu->addAction(tr("Priority Down"));
  prio_down->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Down));
  connect(prio_down, &QAction::triggered, this,
          &AppMenuBar::priority_down_requested);
}

// --------- View ---------

void AppMenuBar::build_view_menu() {
  auto *menu = addMenu(tr("&View"));

  toggle_toolbar_action_ = menu->addAction(tr("Show Toolbar"));
  toggle_toolbar_action_->setCheckable(true);
  toggle_toolbar_action_->setChecked(true);
  connect(toggle_toolbar_action_, &QAction::toggled, this,
          &AppMenuBar::toggle_toolbar);

  toggle_status_bar_action_ = menu->addAction(tr("Show Status Bar"));
  toggle_status_bar_action_->setCheckable(true);
  toggle_status_bar_action_->setChecked(true);
  connect(toggle_status_bar_action_, &QAction::toggled, this,
          &AppMenuBar::toggle_status_bar);

  toggle_console_action_ = menu->addAction(tr("Show Console"));
  toggle_console_action_->setCheckable(true);
  toggle_console_action_->setChecked(false);
  toggle_console_action_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Backtab));
  connect(toggle_console_action_, &QAction::toggled, this,
          &AppMenuBar::toggle_console);

  menu->addSeparator();

  auto *pipeline = menu->addAction("Workflow Pipeline...");
  pipeline->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_P));
  connect(pipeline, &QAction::triggered, this, &AppMenuBar::pipeline_requested);

  menu->addSeparator();

  auto *icons_menu = menu->addMenu(tr("Icons"));
  icons_menu_ = icons_menu;
  auto *icons_group = new QActionGroup(this);
  icons_group->setExclusive(true);
  auto add_icon_size = [&](const QString &label, int size, bool checked) {
    auto *act = icons_menu->addAction(label);
    act->setCheckable(true);
    act->setData(size);
    act->setChecked(checked);
    icons_group->addAction(act);
    connect(act, &QAction::triggered, this,
            [this, size]() { emit icon_size_requested(size); });
  };
  add_icon_size(tr("Small"), 24, true);
  add_icon_size(tr("Medium"), 32, false);
  add_icon_size(tr("Large"), 48, false);

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
  connect(sort_action_, &QAction::triggered, this,
          &AppMenuBar::sort_mods_requested);
}

void AppMenuBar::update_tools_for_game(
    const std::string &game_id,
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
      connect(act, &QAction::triggered, this,
              [this, tid, gid]() { emit tool_requested(tid, gid); });
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

// --------- Help ---------

void AppMenuBar::build_help_menu() {
  auto *menu = addMenu(tr("&Help"));

  auto *stats = menu->addAction(tr("Instance Statistics..."));
  connect(stats, &QAction::triggered, this,
          &AppMenuBar::instance_statistics_requested);

  menu->addSeparator();

  auto *about = menu->addAction(tr("About GameModManager"));
  connect(about, &QAction::triggered, this, &AppMenuBar::about_requested);

  auto *about_qt = menu->addAction(tr("About Qt"));
  connect(about_qt, &QAction::triggered, this, &AppMenuBar::about_qt_requested);

  menu->addSeparator();
}

// --------- Recent instances ---------

void AppMenuBar::set_recent_instances(
    const std::vector<std::string> &instances) {
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

} // namespace ui
