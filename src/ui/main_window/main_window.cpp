#include "ui/main_window/main_window.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QResizeEvent>
#include <QShortcut>
#include <QSplitter>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>
#include <QToolBar>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include "ui/controllers/downloads_controller.h"
#include "ui/controllers/launch_controller.h"
#include "ui/controllers/mod_list_controller.h"
#include "ui/controllers/overwrite_controller.h"
#include "ui/controllers/queue_controller.h"
#include "ui/controllers/settings_controller.h"
#include "ui/controllers/tab_mode_controller.h"
#include "ui/panels/tab_panels.h"
#include "ui/settings/settings.h"
#include "ui/system_tray/system_tray_manager.h"
#include "ui/system_tray/tray_decision.h"
#include "ui/theme/icon_manager.h"
#include "ui/widgets/console_panel.h"
#include "ui/widgets/debug_window.h"
#include "ui/widgets/exec_controls_bar.h"
#include "ui/widgets/game_path_banner.h"
#include "ui/widgets/main_tab_container.h"
#include "ui/widgets/main_toolbar.h"
#include "ui/widgets/menu_bar.h"
#include "ui/widgets/mod_filter_bar.h"
#include "ui/widgets/mod_table_view.h"
#include "ui/widgets/profile_bar.h"
#include "ui/widgets/right_filter_bar.h"
#include "ui/widgets/right_panel.h"
#include "ui/widgets/smooth_scroll.h"
#include "ui/widgets/status_bar.h"
#include "ui/workers/pipeline_worker.h"

#include "engine/events/event_bus.h"

namespace ui {

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
  setWindowTitle(tr("GameModManager"));
  resize(1200, 800);
  setAcceptDrops(true);

  // Issue #16 controllers - the composer delegates behavior to these. Each
  // controller reaches the shared members below through w_-> (friend).
  launch_    = std::make_unique<LaunchController>(this, this);
  queue_     = std::make_unique<QueueController>(this, this);
  overwrite_ = std::make_unique<OverwriteController>(this, this);
  downloads_ = std::make_unique<DownloadsController>(this, this);
  mod_list_  = std::make_unique<ModListController>(this, this);
  settings_  = std::make_unique<SettingsController>(this, this);
  // Full-UI tab host: created before TabModeController so it can connect to
  // the container in its ctor; the Main tab is added once the console
  // splitter exists (below).
  main_tab_container_ = new MainTabContainer(this);
  tab_mode_           = std::make_unique<TabModeController>(this, this);

  // UI Locker for disabling/enabling the interface during operations
  locker_ = std::make_unique<Locker>(this);

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
  toolbar_      = new MainToolbar(this);
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

  connect(toolbar_, &MainToolbar::settings_clicked, tab_mode_.get(),
          &TabModeController::route_settings);
  connect(toolbar_, &MainToolbar::instances_clicked, tab_mode_.get(),
          &TabModeController::route_instance_switcher);
  connect(menu_bar_, &AppMenuBar::sort_mods_requested, mod_list_.get(),
          &ModListController::sort_mods);
  connect(toolbar_, &MainToolbar::shortcut_removed, this, [this](const QString &path) {
    int idx = toolbar_shortcut_paths_.indexOf(path);
    if (idx >= 0)
      toolbar_shortcut_paths_.removeAt(idx);
    mod_list_->save_order();
  });

  // --- Instance Options button: body opens the Instance Options panel, arrow
  // the menu ---
  {
    QIcon instance_options_icon =
        engine::IconManager::instance().resolve_icon("proton", QStyle::SP_ComputerIcon);
    toolbar_->add_instance_options_button(instance_options_icon);

    auto *instance_options_menu = new QMenu(this);
    instance_options_menu->addAction(tr("Run winecfg"), this, [this]() {
      launch_->run_prefix_tool({"winecfg"});
    });
    instance_options_menu->addAction(tr("Run winetricks"), this, [this]() {
      launch_->run_prefix_tool({});
    });
    instance_options_menu->addAction(tr("Run an .exe in this prefix..."), this,
                                     [this]() {
                                       launch_->run_exe_in_prefix();
                                     });

    instance_options_menu->addSeparator();

    instance_options_menu->addAction(tr("Open Wine Registry"), this, [this]() {
      launch_->run_prefix_tool({"regedit"});
    });
    instance_options_menu->addAction(tr("Install a DLL..."), this, [this]() {
      // winetricks `dlls` lands straight on the "Install a Windows DLL
      // or component" picker.
      launch_->run_prefix_tool({"dlls"});
    });

    instance_options_menu->addSeparator();

    instance_options_menu->addAction(tr("Install recommended packages"), this,
                                     [this]() {
                                       tab_mode_->route_instance_options();
                                     });

    toolbar_->set_instance_options_menu(instance_options_menu);
    connect(toolbar_, &MainToolbar::instance_options_clicked, tab_mode_.get(),
            &TabModeController::route_instance_options);
  }

  // --- Vertical splitter: main area + console (console hidden by default) ---
  console_splitter_ = new QSplitter(Qt::Vertical, this);

  // Main horizontal area
  auto *main_area   = new QWidget(this);
  auto *main_layout = new QVBoxLayout(main_area);
  main_layout->setContentsMargins(0, 0, 0, 0);
  main_layout->setSpacing(0);

  // "Set Game Path" banner (Workspace-tnj): hidden until a game-less
  // instance is loaded; sits above the splitter so it is always visible.
  game_path_banner_ = new GamePathBanner(main_area);
  game_path_banner_->setWhatsThis(
      tr("This instance has no game directory yet. Pick one so the app can read the "
         "game's own files and launch it."));
  main_layout->addWidget(game_path_banner_);

  // --- Left panel: profile bar, mod list, filter bar stacked vertically.
  // ModListController::setup_mod_list fills it (Issue #16). ---
  auto *left_panel  = new QWidget(this);
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

  // Ctrl+F / Escape for the filter bars - registered here because both bars
  // exist from this point on (the mod list's in setup_mod_list above, the
  // right panel's two lines down).
  setup_filter_shortcuts();

  console_splitter_->addWidget(main_area);

  // --- Console panel (hidden by default, drag to expand) ---
  console_ = new ConsolePanel(this);
  console_->setWhatsThis(
      tr("The log window. It stays collapsed until \"View > Show Console\" turns it "
         "on; drag the divider to give it more room."));
  console_->setMinimumHeight(0);
  console_->setMaximumHeight(300);
  console_splitter_->addWidget(console_);

  console_splitter_->setStretchFactor(0, 1);
  console_splitter_->setStretchFactor(1, 0);
  console_splitter_->setSizes({700, 0});

  // Full-UI tab host: the console splitter becomes the permanent Main tab;
  // dynamic view tabs (Settings, Pipeline, ...) are added on top when Full
  // UI mode is ON. With the mode OFF the tab bar stays hidden and the window
  // looks exactly like the pre-tab layout.
  main_tab_container_->add_main_tab(console_splitter_);
  setCentralWidget(main_tab_container_);

  // Game-lock overlay (hidden until game launches)
  launch_->create_game_lock_overlay();

  // --- Status bar ---
  status_bar_ = new StatusBar(this);
  status_bar_->setWhatsThis(
      tr("Shows what the app is doing, and the sources the active instance draws "
         "mods from."));
  statusBar()->addWidget(status_bar_, 1);

  // Global event filter for Konami code (child widgets may eat arrow keys).
  // The filter logic lives in SettingsController::handle_global_event.
  QApplication::instance()->installEventFilter(this);
  setFocusPolicy(Qt::StrongFocus);

  // --- System tray ---
  // Tray click or the menu's Show brings the window back. Quit runs the real
  // close path rather than qApp->quit() so the download manifest, app state
  // and mod order are still saved.
  tray_ = new SystemTrayManager(this);
  tray_->show();
  connect(tray_, &SystemTrayManager::activate_requested, this, [this] {
    restore_window();
  });
  connect(tray_, &SystemTrayManager::quit_requested, this, [this] {
    quitting_ = true;
    close();
  });

  // A game finishing while we are in the tray brings the window back
  // (MO2 mainwindow.cpp:488-492). kGameFinished is our onFinishedRun.
  game_finished_sub_ = engine::EventBus::instance().subscribe(
      engine::events::kGameFinished, [this](const std::string &, const std::string &) {
        if (isHidden())
          restore_window();
      });

  // --- Pipeline thread, source providers, download/install signals ---
  downloads_->setup_pipeline();

  connect(right_panel_->exec_controls(), &ExecControlsBar::run_clicked, launch_.get(),
          &LaunchController::launch_game);

  // LOOT sort shortcut from the Plugins tab filter bar
  connect(right_panel_, &RightPanel::sort_requested, mod_list_.get(),
          &ModListController::run_loot_sort);

  connect(right_panel_->exec_controls(), &ExecControlsBar::shortcut_to_toolbar,
          launch_.get(), &LaunchController::add_shortcut_to_toolbar);

  connect(right_panel_->exec_controls(), &ExecControlsBar::shortcut_to_desktop,
          launch_.get(), &LaunchController::add_shortcut_to_desktop);

  // "<Edit...>" in the executables combo opens the executable editor. Routed
  // through TabModeController so Full UI mode embeds it as a tab and popup
  // mode keeps the modal Executables::Dialog.
  connect(right_panel_->exec_controls(), &ExecControlsBar::add_entry_requested,
          tab_mode_.get(), &TabModeController::route_exec_entry);

  // Keep the persisted per-instance selection in sync with the live combo
  connect(right_panel_->exec_controls(), &ExecControlsBar::current_executable_changed,
          this, [this]() {
            pending_exec_selection_ =
                right_panel_->exec_controls()->current_executable();
          });

  // Persist the last selected right-panel tab per instance (Issue #21).
  // write_key does a read-before-write of instance.toml, so app-owned keys
  // (executables, ...) survive. The in-memory instance is refreshed so the
  // value is immediately visible to restore_tab on the next switch.
  connect(
      right_panel_, &RightPanel::tab_changed, this, [this](const QString &capability) {
        if (current_instance_root_.empty())
          return;
        engine::Instance write = engine::Instance::from_root(current_instance_root_);
        write.read_toml();
        write.write_key("last_tab", capability.toStdString());
        write.info().last_tab = capability.toStdString();
        current_instance_     = write;
      });

  // Lazy right-panel tabs (Workspace-j6ty): a tab's content is built on
  // first show, so the per-tab wiring that used to run eagerly after
  // set_game() runs here instead - exactly once per tab build. The Data
  // tab is always present and wired directly by the controllers.
  connect(right_panel_, &RightPanel::tab_materialized, this,
          [this](const QString &capability) {
            const auto cap = capability.toStdString();
            if (cap == "downloads") {
              // Manifest, downloads dir + watchdog, signal connections.
              // The downloads dir is instance-owned, so this works without
              // a game path.
              downloads_->wire_downloads_tab();
            } else if (cap == "saves") {
              // Points the tab at the game's saves dir and connects
              // refresh/delete; the scan itself stays lazy (first show).
              // Gated on knowledge_ like the old game-load wiring was.
              if (knowledge_) {
                downloads_->wire_saves_tab();
              }
            } else if (cap == "conflicts") {
              if (auto *ct = right_panel_->conflicts_tab()) {
                connect(ct, &ui::ConflictsTab::image_diff_requested, mod_list_.get(),
                        &ModListController::on_image_diff_requested);
                connect(ct, &ui::ConflictsTab::file_open_requested, mod_list_.get(),
                        &ModListController::on_conflict_file_open);
                connect(ct, &ui::ConflictsTab::file_preview_requested, mod_list_.get(),
                        &ModListController::on_conflict_file_preview);
                connect(ct, &ui::ConflictsTab::file_reveal_requested, mod_list_.get(),
                        &ModListController::on_conflict_file_reveal);
              }
              mod_list_->refresh_conflicts_tab();
            } else if (cap == "plugins") {
              // Connects the tab's signals (first sight of the new
              // instance) and populates it from the current model.
              mod_list_->refresh_plugins_tab();
            }
          });

  // Banner picker (Workspace-tnj): the host owns the picker so the
  // launch/deploy guards reuse the same flow (prompt_for_game_path).
  connect(game_path_banner_, &GamePathBanner::pick_requested, this, [this]() {
    prompt_for_game_path();
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
MainWindow::~MainWindow() {
  // The EventBus is a process-wide singleton, so this subscription would
  // outlive the window and fire into freed memory if a game-finished event
  // landed during shutdown.
  if (game_finished_sub_ != 0)
    engine::EventBus::instance().unsubscribe(game_finished_sub_);
}

void MainWindow::setup_filter_shortcuts() {
  // MO2 parity (G15). Nothing else in the UI claims Ctrl+F or Escape - the
  // menu bar binds New/Open/Preferences/Quit/SelectAll/Refresh plus a handful
  // of explicit Ctrl+<letter>s, and Escape is only handled by dialogs, which
  // own their own window and so are outside a WindowShortcut context.
  auto *find_filter = new QShortcut(QKeySequence::Find, this);
  find_filter->setAutoRepeat(false);
  connect(find_filter, &QShortcut::activated, this, &MainWindow::focus_active_filter);

  auto *clear_filter = new QShortcut(QKeySequence(Qt::Key_Escape), this);
  clear_filter->setAutoRepeat(false);
  connect(clear_filter, &QShortcut::activated, this, &MainWindow::clear_active_filter);
}

void MainWindow::focus_active_filter() {
  // MO2 gives each list its own pair of hooks (mainwindow.cpp:464-466); here
  // one window-scoped pair serves both bars, so the bar the user is already
  // in wins and the mod list is the fallback.
  if (right_panel_ && right_panel_->filter_bar()->filter_has_focus())
    right_panel_->filter_bar()->focus_filter();
  else if (filter_bar_)
    filter_bar_->focus_filter();
}

void MainWindow::clear_active_filter() {
  if (right_panel_ && right_panel_->filter_bar()->filter_has_focus()) {
    // MO2 hands focus back to the list that owns the filter. GMM's right
    // panel shares ONE bar across its lazily built tabs (plugins, downloads,
    // conflicts, saves, data), so there is no single view to return to -
    // staying in the right panel is the honest equivalent, and stopping here
    // keeps the mod list from stealing focus across the window.
    right_panel_->filter_bar()->clear_filter();
    return;
  }
  if (filter_bar_) {
    // Unconditional, exactly like MO2's reset lambda (edit->clear();
    // widget->setFocus(); - mainwindow.cpp:211-214). Gating the hand-back on
    // "was there text to clear" left the keyboard stuck in an empty filter.
    filter_bar_->clear_filter();
    mod_view_->setFocus();
  }
}

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

void MainWindow::set_active_profile(
    std::unique_ptr<engine::profile::ProfileManager> profile) {
  active_profile_ = std::move(profile);
  if (debug_window_)
    debug_window_->set_active_profile(active_profile_.get());
}

bool MainWindow::prompt_for_game_path() {
  const QString dir =
      QFileDialog::getExistingDirectory(this, tr("Choose game directory"));
  if (dir.isEmpty() || current_instance_root_.empty())
    return false;
  // write_key read-modify-writes the whole file, so app-owned sections
  // ([executables], ...) survive; write_toml() would clobber them.
  engine::Instance::from_root(current_instance_root_)
      .write_key("game_dir", dir.toStdString());
  settings_->set_game_info(current_game_id_, current_game_name_, current_profile_name_,
                           dir.toStdString(), current_instance_root_);
  return true;
}

void MainWindow::handle_nxm_download(const engine::NxmLink &link) {
  downloads_->handle_nxm_download(link);
}

void MainWindow::handle_modl_download(const engine::Source::ModlLink &link) {
  downloads_->handle_modl_download(link);
}

void MainWindow::on_notification(const QString &title, const QString &message) {
  status_bar_->set_status(title + ": " + message);
  // MO2 also raises a tray balloon for these (downloadmanager.cpp:1826, :2406).
  // QSystemTrayIcon::showMessage is already a no-op without a tray.
  tray_->show_notification(title, message);
}

void MainWindow::update_title() {
  if (current_game_name_.empty()) {
    setWindowTitle(tr("GameModManager"));
  } else if (current_profile_name_.empty()) {
    setWindowTitle(("GameModManager - " + current_game_name_).c_str());
  } else {
    setWindowTitle(
        ("GameModManager - " + current_profile_name_ + " - " + current_game_name_)
            .c_str());
  }

  // The status bar's context label names the same identity in the order
  // MO2's updateNormalMessage() does (statusbar.cpp:144-163): game, instance,
  // profile. Driven from here because this is the one place every identity
  // change funnels through - an instance load, a profile switch, and the
  // profile-name fallback all call update_title().
  if (status_bar_ != nullptr) {
    status_bar_->set_context(
        ui::context_label_text(QString::fromStdString(current_game_name_),
                               QString::fromStdString(current_instance_root_
                                                          .filename()
                                                          .string()),
                               QString::fromStdString(current_profile_name_)));
  }
}

void MainWindow::resizeEvent(QResizeEvent *event) {
  QMainWindow::resizeEvent(event);
  if (game_lock_overlay_)
    game_lock_overlay_->setGeometry(rect());
  if (drop_overlay_ && drop_overlay_->isVisible())
    drop_overlay_->setGeometry(rect());
}

namespace {
  // A drop is a modpack drop when any dragged URL is a local .gmmpack/.zip.
  bool is_pack_drop(const QMimeData *mime) {
    if (mime == nullptr || !mime->hasUrls())
      return false;
    for (const QUrl &url : mime->urls()) {
      const QString suffix = QFileInfo(url.toLocalFile()).suffix().toLower();
      if (suffix == "gmmpack" || suffix == "zip")
        return true;
    }
    return false;
  }
}  // namespace

void MainWindow::dragEnterEvent(QDragEnterEvent *event) {
  if (!is_pack_drop(event->mimeData()))
    return;
  event->acceptProposedAction();
  set_drop_overlay_visible(true);
}

void MainWindow::dragLeaveEvent(QDragLeaveEvent *event) {
  set_drop_overlay_visible(false);
  QMainWindow::dragLeaveEvent(event);
}

void MainWindow::dropEvent(QDropEvent *event) {
  set_drop_overlay_visible(false);
  if (!is_pack_drop(event->mimeData()))
    return;
  for (const QUrl &url : event->mimeData()->urls()) {
    const QString local  = url.toLocalFile();
    const QString suffix = QFileInfo(local).suffix().toLower();
    if (!local.isEmpty() && (suffix == "gmmpack" || suffix == "zip")) {
      event->acceptProposedAction();
      settings_->import_modpack(local);
      return;
    }
  }
}

void MainWindow::set_drop_overlay_visible(bool visible) {
  if (!visible) {
    if (drop_overlay_ != nullptr)
      drop_overlay_->hide();
    return;
  }
  if (drop_overlay_ == nullptr) {
    drop_overlay_ = new QLabel(this);
    drop_overlay_->setAlignment(Qt::AlignCenter);
    drop_overlay_->setWordWrap(true);
    drop_overlay_->setAttribute(Qt::WA_TransparentForMouseEvents);
    QFont font = drop_overlay_->font();
    font.setPointSize(font.pointSize() + 8);
    font.setBold(true);
    drop_overlay_->setFont(font);
    const QPalette palette = this->palette();
    const QColor bg        = palette.color(QPalette::Highlight);
    const QColor fg        = palette.color(QPalette::HighlightedText);
    drop_overlay_->setStyleSheet(
        QStringLiteral("background-color: rgba(%1, %2, %3, 160); color: %4; "
                       "border: 3px dashed %4; border-radius: 12px;")
            .arg(bg.red())
            .arg(bg.green())
            .arg(bg.blue())
            .arg(fg.name()));
  }
  drop_overlay_->setText(tr("Drop to import modpack (.gmmpack)"));
  drop_overlay_->setGeometry(rect());
  drop_overlay_->raise();
  drop_overlay_->show();
}

void MainWindow::set_ui_enabled(bool enabled) {
  // Delegate to the UI Locker which handles the actual enable/disable logic
  locker_->set_enabled(enabled);
}

void MainWindow::restore_window() {
  showNormal();
  raise();
  activateWindow();
}

void MainWindow::closeEvent(QCloseEvent *event) {
  // Minimize-to-tray is decided before anything destructive runs: the app
  // keeps running, so the manifest, settings and mod order must not be written
  // and the pipeline must not be stopped.
  const auto action = ui::tray::decide_tray_action(
      {tray_->is_available(), Settings::instance().minimize_to_tray(), quitting_},
      ui::tray::TrayIntent::CloseWindow);
  if (action == ui::tray::TrayAction::HideToTray) {
    event->ignore();
    hide();
    return;
  }

  // Ask before closing with active downloads (downloads_->confirm_close);
  // Cancel aborts the close, everything else falls through.
  if (!downloads_->confirm_close()) {
    event->ignore();
    // A tray Quit the user then declined must not latch: the next window close
    // has to ask again rather than sail straight through.
    quitting_ = false;
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

  // A visible QSystemTrayIcon suppresses QApplication's quit-on-last-window-
  // closed, so the window would hide and the process would stay resident with
  // no way back in. We are committed to quitting now, so drop the tray icon.
  tray_->hide();

  QMainWindow::closeEvent(event);
}

bool MainWindow::eventFilter(QObject *obj, QEvent *event) {
  if (settings_->handle_global_event(obj, event))
    return true;

  // Sync View menu checkboxes when panel visibility changes by any means
  // (splitter drag, programmatic show/hide, window state restore, etc.)
  if (menu_bar_ && (event->type() == QEvent::Show || event->type() == QEvent::Hide)) {
    if (obj == toolbar_area_) {
      menu_bar_->set_toolbar_checked(event->type() == QEvent::Show);
    } else if (obj == statusBar()) {
      menu_bar_->set_status_bar_checked(event->type() == QEvent::Show);
    } else if (obj == console_) {
      menu_bar_->set_console_checked(event->type() == QEvent::Show);
    }
  }

  return QMainWindow::eventFilter(obj, event);
}

void MainWindow::apply_initial_geometry() {
  if (!pending_geometry_.isEmpty()) {
    restoreGeometry(pending_geometry_);
    pending_geometry_.clear();
  }
}

}  // namespace ui

#include "moc_main_window.cpp"
