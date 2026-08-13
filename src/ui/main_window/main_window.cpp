#include "ui/main_window/main_window.h"

#include <QApplication>
#include <QCloseEvent>
#include <QEvent>
#include <QIcon>
#include <QMenu>
#include <QResizeEvent>
#include <QSplitter>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWidget>

#include "engine/theme/icon_manager.h"
#include "ui/controllers/downloads_controller.h"
#include "ui/controllers/launch_controller.h"
#include "ui/controllers/mod_list_controller.h"
#include "ui/controllers/overwrite_controller.h"
#include "ui/controllers/queue_controller.h"
#include "ui/controllers/settings_controller.h"
#include "ui/pipeline_worker.h"
#include "ui/settings/settings.h"
#include "ui/smooth_scroll.h"
#include "ui/widgets/console_panel.h"
#include "ui/widgets/exec_controls_bar.h"
#include "ui/widgets/gmm_status_bar.h"
#include "ui/widgets/main_toolbar.h"
#include "ui/widgets/menu_bar.h"
#include "ui/widgets/profile_bar.h"
#include "ui/widgets/right_panel.h"

namespace ui {

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
  setWindowTitle(tr("GameModManager"));
  resize(1200, 800);

  // Issue #16 controllers — the composer delegates behavior to these. Each
  // controller reaches the shared members below through w_-> (friend).
  launch_ = std::make_unique<LaunchController>(this, this);
  queue_ = std::make_unique<QueueController>(this, this);
  overwrite_ = std::make_unique<OverwriteController>(this, this);
  downloads_ = std::make_unique<DownloadsController>(this, this);
  mod_list_ = std::make_unique<ModListController>(this, this);
  settings_ = std::make_unique<SettingsController>(this, this);

  // Conflict recompute infra (P8.1, THREADING.md §3.6): debounce + worker
  // thread so toggling/reordering a mod never blocks the UI on a full scan.
  conflict_debounce_timer_ = new QTimer(this);
  conflict_debounce_timer_->setSingleShot(true);
  conflict_debounce_timer_->setInterval(150);
  connect(conflict_debounce_timer_, &QTimer::timeout, mod_list_.get(),
          &ModListController::start_conflict_scan);

  // --- Menu bar (must be created before the toolbar so parent is set) ---
  settings_->setup_menu_bar();

  // --- Toolbar: QToolBar handles docking, orientation, and sizing natively ---
  toolbar_ = new MainToolbar(this);
  toolbar_area_ = new QToolBar(this);
  toolbar_area_->setObjectName("MainToolbar");
  toolbar_area_->setMovable(true);
  toolbar_area_->setFloatable(true);
  toolbar_area_->setIconSize(QSize(24, 24));
  toolbar_area_->setContextMenuPolicy(Qt::PreventContextMenu);
  toolbar_area_->addWidget(toolbar_);
  addToolBar(toolbar_area_);

  // QToolBar tells us when orientation changes (horizontal ↔ vertical)
  connect(toolbar_area_, &QToolBar::orientationChanged, this,
          [this](Qt::Orientation orient) {
            toolbar_->set_vertical(orient == Qt::Vertical);
          });

  connect(toolbar_, &MainToolbar::settings_clicked, settings_.get(),
          &SettingsController::show_settings_dialog);
  connect(toolbar_, &MainToolbar::instances_clicked, settings_.get(),
          &SettingsController::show_instance_switcher);
  connect(menu_bar_, &AppMenuBar::sort_mods_requested, mod_list_.get(),
          &ModListController::sort_mods);
  connect(toolbar_, &MainToolbar::shortcut_removed, this,
          [this](const QString &path) {
            int idx = toolbar_shortcut_paths_.indexOf(path);
            if (idx >= 0) {
              toolbar_shortcut_paths_.removeAt(idx);
              toolbar_shortcut_icons_.removeAt(idx);
            }
            mod_list_->save_order();
          });

  // --- Proton button: body opens the Proton options panel, arrow the menu ---
  {
    QIcon proton_icon = engine::IconManager::instance().resolve_icon(
        "proton", QStyle::SP_ComputerIcon);
    toolbar_->add_proton_button(proton_icon);

    auto *proton_menu = new QMenu(this);
    proton_menu->addAction(tr("Run winecfg"), this,
                           [this]() { launch_->run_prefix_tool({"winecfg"}); });
    proton_menu->addAction(tr("Run winetricks"), this,
                           [this]() { launch_->run_prefix_tool({}); });
    proton_menu->addAction(tr("Run an .exe in this prefix..."), this,
                           [this]() { launch_->run_exe_in_prefix(); });

    proton_menu->addSeparator();

    proton_menu->addAction(tr("Open Wine Registry"), this,
                           [this]() { launch_->run_prefix_tool({"regedit"}); });
    proton_menu->addAction(tr("Install a DLL..."), this, [this]() {
      // winetricks `dlls` lands straight on the "Install a Windows DLL
      // or component" picker.
      launch_->run_prefix_tool({"dlls"});
    });

    proton_menu->addSeparator();

    proton_menu->addAction(tr("Install recommended packages"), this,
                           [this]() { launch_->show_proton_panel(); });

    toolbar_->set_proton_menu(proton_menu);
    connect(toolbar_, &MainToolbar::proton_clicked, launch_.get(),
            &LaunchController::show_proton_panel);
  }

  // --- Vertical splitter: main area + console (console hidden by default) ---
  console_splitter_ = new QSplitter(Qt::Vertical, this);

  // Main horizontal area
  auto *main_area = new QWidget(this);
  auto *main_layout = new QVBoxLayout(main_area);
  main_layout->setContentsMargins(0, 0, 0, 0);
  main_layout->setSpacing(0);

  // --- Left panel: profile bar, mod list, filter bar stacked vertically.
  // ModListController::setup_mod_list fills it (Issue #16). ---
  auto *left_panel = new QWidget(this);
  auto *left_layout = new QVBoxLayout(left_panel);
  left_layout->setContentsMargins(0, 0, 0, 0);
  left_layout->setSpacing(0);
  mod_list_->setup_mod_list(left_layout);

  main_splitter_ = new QSplitter(Qt::Horizontal, this);
  main_splitter_->addWidget(left_panel);

  main_layout->addWidget(main_splitter_, 1);

  right_panel_ = new RightPanel(this);
  main_splitter_->addWidget(right_panel_);

  main_splitter_->setStretchFactor(0, 3);
  main_splitter_->setStretchFactor(1, 2);

  console_splitter_->addWidget(main_area);

  // --- Console panel (hidden by default, drag to expand) ---
  console_ = new ConsolePanel(this);
  console_->setMinimumHeight(0);
  console_->setMaximumHeight(300);
  console_splitter_->addWidget(console_);

  console_splitter_->setStretchFactor(0, 1);
  console_splitter_->setStretchFactor(1, 0);
  console_splitter_->setSizes({700, 0});

  setCentralWidget(console_splitter_);

  // Game-lock overlay (hidden until game launches)
  launch_->create_game_lock_overlay();

  // --- Status bar ---
  status_bar_ = new GmmStatusBar(this);
  statusBar()->addWidget(status_bar_, 1);

  // Global event filter for Konami code (child widgets may eat arrow keys).
  // The filter logic lives in SettingsController::handle_global_event.
  QApplication::instance()->installEventFilter(this);
  setFocusPolicy(Qt::StrongFocus);

  // --- Pipeline thread, source providers, download/install signals ---
  downloads_->setup_pipeline();

  connect(right_panel_->exec_controls(), &ExecControlsBar::run_clicked,
          launch_.get(), &LaunchController::launch_game);

  // LOOT sort shortcut from the Plugins tab filter bar
  connect(right_panel_, &RightPanel::sort_requested, mod_list_.get(),
          &ModListController::run_loot_sort);

  connect(right_panel_->exec_controls(), &ExecControlsBar::shortcut_to_toolbar,
          launch_.get(), &LaunchController::add_shortcut_to_toolbar);

  connect(right_panel_->exec_controls(), &ExecControlsBar::shortcut_to_desktop,
          launch_.get(), &LaunchController::add_shortcut_to_desktop);

  connect(right_panel_->exec_controls(), &ExecControlsBar::add_entry_requested,
          launch_.get(), &LaunchController::on_add_entry_requested);

  // Keep the persisted per-instance selection in sync with the live combo
  connect(right_panel_->exec_controls(),
          &ExecControlsBar::current_executable_changed, this, [this]() {
            pending_exec_selection_ =
                right_panel_->exec_controls()->current_executable();
          });

  // Persist the last selected right-panel tab per instance (Issue #21).
  // write_key does a read-before-write of instance.toml, so app-owned keys
  // (executables, ...) survive. The in-memory instance is refreshed so the
  // value is immediately visible to restore_tab on the next switch.
  connect(right_panel_, &RightPanel::tab_changed, this,
          [this](const QString &capability) {
            if (current_instance_root_.empty())
              return;
            engine::Instance write =
                engine::Instance::from_root(current_instance_root_);
            write.read_toml();
            write.write_key("last_tab", capability.toStdString());
            write.info().last_tab = capability.toStdString();
            current_instance_ = write;
          });

  // Start IPC server to receive nxm:// URLs from other GMM processes
  downloads_->setup_nxm_ipc();

  settings_->connect_menu_actions();
  mod_list_->setup_mod_list_context_menu();

  // Populate Recent Instances submenu
  settings_->refresh_recent_instances();

  // Smooth scrolling on all item views (mod list, right-panel tables).
  if (Settings::instance().smooth_scrolling())
    ui::enable_smooth_scrolling(this);
}

// Out-of-line so the unique_ptr controller members (ModListController,
// SettingsController, LaunchController, ...) are destroyed here, where every
// controller header is included (complete types are available).
MainWindow::~MainWindow() = default;

void MainWindow::set_game_info(const std::string &game_id,
                               const std::string &game_display_name,
                               const std::string &profile_name,
                               const std::filesystem::path &game_dir,
                               const std::filesystem::path &instance_root) {
  // Instance/session setup (state reset, right-panel rebuild, pipeline
  // config, app-state restore) lives in SettingsController::set_game_info.
  settings_->set_game_info(game_id, game_display_name, profile_name, game_dir,
                           instance_root);
}

void MainWindow::handle_nxm_download(const engine::NxmLink &link) {
  downloads_->handle_nxm_download(link);
}

void MainWindow::on_notification(const QString &title, const QString &message) {
  status_bar_->set_status(title + ": " + message);
}

void MainWindow::update_title() {
  if (current_game_name_.empty()) {
    setWindowTitle(tr("GameModManager"));
  } else if (current_profile_name_.empty()) {
    setWindowTitle(("GameModManager - " + current_game_name_).c_str());
  } else {
    setWindowTitle(("GameModManager - " + current_profile_name_ + " - " +
                    current_game_name_)
                       .c_str());
  }
}

void MainWindow::resizeEvent(QResizeEvent *event) {
  QMainWindow::resizeEvent(event);
  if (game_lock_overlay_)
    game_lock_overlay_->setGeometry(rect());
}

void MainWindow::set_ui_enabled(bool enabled) {
  // Lock or unlock the whole manager surface (mod list, panels, console,
  // menus, toolbars). The install dialogs (FOMOD wizard, name confirm,
  // overwrite query, progress popup) are top-level children of `this`, NOT
  // of the disabled content widgets, so they stay interactive while the
  // manager itself is greyed out - the same shape MO2's UILocker produces.
  if (centralWidget())
    centralWidget()->setEnabled(enabled);
  if (menu_bar_)
    menu_bar_->setEnabled(enabled);
  if (toolbar_area_)
    toolbar_area_->setEnabled(enabled);
  if (profile_bar_)
    profile_bar_->setEnabled(enabled);
  if (status_bar_)
    status_bar_->setEnabled(enabled);
}

void MainWindow::closeEvent(QCloseEvent *event) {
  // Ask before closing with active downloads (downloads_->confirm_close);
  // Cancel aborts the close, everything else falls through.
  if (!downloads_->confirm_close()) {
    event->ignore();
    return;
  }

  downloads_->save_download_manifest();
  settings_->save_app_state();

  // Disconnect pipeline signals to prevent callbacks on a partially-destroyed
  // MainWindow
  if (pipeline_thread_) {
    disconnect(pipeline_thread_->worker(), nullptr, this, nullptr);
    pipeline_thread_->stop();
  }

  // Save mod order before closing
  if (!loading_) {
    mod_list_->save_order();
  }

  QMainWindow::closeEvent(event);
}

bool MainWindow::eventFilter(QObject *obj, QEvent *event) {
  if (settings_->handle_global_event(obj, event))
    return true;
  return QMainWindow::eventFilter(obj, event);
}

void MainWindow::apply_initial_geometry() {
  if (!pending_geometry_.isEmpty()) {
    restoreGeometry(pending_geometry_);
    pending_geometry_.clear();
  }
}

} // namespace ui

#include "moc_main_window.cpp"
