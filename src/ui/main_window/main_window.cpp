#include <algorithm>
#include "ui/main_window/main_window.h"
#include "ui/widgets/mod_list_model.h"
#include "ui/widgets/mod_table_view.h"
#include "ui/widgets/main_toolbar.h"
#include "ui/widgets/profile_bar.h"
#include "ui/widgets/mod_filter_bar.h"
#include "ui/widgets/column_toggle_header.h"
#include "ui/widgets/right_panel.h"
#include "ui/widgets/exec_controls_bar.h"
#include "ui/widgets/console_panel.h"
#include "ui/widgets/gmm_status_bar.h"
#include "ui/widgets/menu_bar.h"
#include "ui/widgets/instance_statistics_dialog.h"
#include "ui/widgets/instance_switcher_dialog.h"
#include "ui/pipeline_worker.h"
#include "ui/panels/tab_panels.h"
#include "engine/launcher.h"
#include "engine/log/logger.h"
#include "engine/detect/mod_scanner.h"
#include "engine/detect/game_detector.h"
#include "engine/index/conflict_engine.h"
#include "engine/registry/game_knowledge.h"
#include "engine/instance/instance.h"
#include "engine/instance/instance_utils.h"
#include "engine/nxm/nxm_router.h"
#include "engine/nxm/managed_games.h"
#include "engine/nxm/nxm_ipc.h"
#include "engine/pipeline/sync_stage.h"
#include "engine/detect/game_detector.h"
#include "ui/game_selection/game_selection_widget.h"

#ifdef GMM_PLATFORM_LINUX
#include "platform/linux/linux_platform.h"
#include "engine/overlay_launcher.h"
#include "engine/preload_interceptor.h"
#include "engine/deploy/strategy.h"
#endif
#include "engine/sort/sort_registry.h"
#include "engine/sort/sort_provider.h"
#include "ui/widgets/debug_window.h"
#include "engine/plugin_host/plugin_loader.h"
#include "engine/theme/style_manager.h"
#include "engine/source/nexus_provider.h"
#include "engine/nexus_auth.h"
#include "engine/source/steam_workshop_provider.h"
#include "engine/pipeline/fetch_stage.h"
#include "engine/pipeline/extract_stage.h"
#include "engine/pipeline/install_stage.h"
#include "engine/pipeline/deploy_stage.h"
#include "engine/deploy/strategy.h"
#include "runtime/runtime.h"

#include <QAction>
#include <algorithm>
#include <cstdarg>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QKeyEvent>
#include <QColorDialog>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFile>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QHeaderView>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

#ifndef _WIN32
#include <signal.h>
#include <cstring>
#include <dirent.h>
#include <sys/types.h>
#endif
#include <QSplitter>
#include <QStatusBar>
#include <QStyle>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTextStream>
#include <QToolBar>
#include <QUrl>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QCheckBox>
#include <QTreeWidget>
#include <QHeaderView>
#include <QGroupBox>
#include <QDialogButtonBox>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <memory>
#include <regex>
#include <sstream>
#include <unordered_set>

namespace ui {

namespace {

// Extract .exe icon via wrestool, fallback to QFileIconProvider — same logic as ExecControlsBar
QIcon extractExeIconShortcut(const QString& exePath) {
    auto app_dir = QCoreApplication::applicationDirPath();
    auto bundled = app_dir + "/../tools/linux/wrestool";
    QString wrestool;
    if (QFileInfo::exists(bundled)) {
        wrestool = bundled;
    } else {
        wrestool = QStandardPaths::findExecutable("wrestool");
    }

    if (!wrestool.isEmpty()) {
        QTemporaryDir tmpDir;
        if (tmpDir.isValid()) {
            auto outIco = tmpDir.filePath("icon.ico");
            QProcess proc;
            proc.start(wrestool, {"-x", "-t", "14", exePath, "-o", outIco});
            if (proc.waitForFinished(3000) && proc.exitCode() == 0) {
                QIcon ico(outIco);
                if (!ico.isNull()) return ico;
            }
        }
    }

    // Try QFileIconProvider (works for installed apps, returns generic for .exe on Linux)
    QFileIconProvider provider;
    auto provider_icon = provider.icon(QFileInfo(exePath));
    if (!provider_icon.isNull()) return provider_icon;

    // Last resort: standard application icon
    return QApplication::style()->standardIcon(QStyle::SP_FileIcon);
}

}  // anonymous namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent) {
    setWindowTitle("GameModManager");
    resize(1200, 800);

    // --- Menu bar (must be created before the toolbar so parent is set) ---
    setup_menu_bar();

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
    connect(toolbar_area_, &QToolBar::orientationChanged, this, [this](Qt::Orientation orient) {
        toolbar_->set_vertical(orient == Qt::Vertical);
    });

    connect(toolbar_, &MainToolbar::settings_clicked, this, &MainWindow::show_settings_dialog);
    connect(toolbar_, &MainToolbar::instances_clicked, this, &MainWindow::show_instance_switcher);
    connect(menu_bar_, &AppMenuBar::sort_mods_requested, this, &MainWindow::sort_mods);
    connect(toolbar_, &MainToolbar::shortcut_removed, this, [this](const QString& path) {
        toolbar_shortcut_paths_.removeAll(path);
        save_order();
    });

    // --- Vertical splitter: main area + console (console hidden by default) ---
    console_splitter_ = new QSplitter(Qt::Vertical, this);

    // Main horizontal area
    auto* main_area = new QWidget(this);
    auto* main_layout = new QVBoxLayout(main_area);
    main_layout->setContentsMargins(0, 0, 0, 0);
    main_layout->setSpacing(0);

    // --- Profile bar above mod list ---
    profile_bar_ = new ProfileBar(this);
    main_layout->addWidget(profile_bar_);

    connect(profile_bar_, &ProfileBar::create_separator_clicked, this, [this]() {
        create_separator();
    });
    connect(profile_bar_, &ProfileBar::create_empty_mod_clicked, this, [this]() {
        QMessageBox::information(this, "Create", "Create Empty Mod - coming soon");
    });

    connect(profile_bar_, &ProfileBar::profile_changed, this, [this](const QString& profile) {
        current_profile_name_ = profile.toStdString();
        update_title();
    });

    // --- Horizontal splitter: mod list + right panel ---
    main_splitter_ = new QSplitter(Qt::Horizontal, this);

    // Left: mod list + filter bar stacked vertically
    auto* left_panel = new QWidget(this);
    auto* left_layout = new QVBoxLayout(left_panel);
    left_layout->setContentsMargins(0, 0, 0, 0);
    left_layout->setSpacing(0);

    mod_model_ = new ModListModel(this);
    mod_view_ = new ModTableView(this);
    mod_model_->set_view(mod_view_);
    mod_view_->setModel(mod_model_);

    // Highlight conflicting mods on selection
    connect(mod_view_->selectionModel(), &QItemSelectionModel::currentChanged,
            this, [this](const QModelIndex& current, const QModelIndex& /*previous*/) {
        if (!current.isValid()) {
            mod_model_->set_selected_mod({});
            return;
        }
        const auto& mods = mod_model_->mods();
        if (current.row() >= 0 && current.row() < mods.size() &&
            !mods[current.row()].is_separator && !mods[current.row()].is_overwrite) {
            mod_model_->set_selected_mod(mods[current.row()].id);
        } else {
            mod_model_->set_selected_mod({});
        }
    });

    // Set alternating row colors via palette (dark-theme-aware)
    auto pal = mod_view_->palette();
    auto base = pal.color(QPalette::Base);
    bool dark = base.lightness() < 128;
    pal.setColor(QPalette::AlternateBase, dark ? base.lighter(120) : base.darker(108));
    mod_view_->setPalette(pal);

    // Sync checkbox toggles to filesystem (disable.it)
    connect(mod_model_, &QAbstractItemModel::dataChanged,
            this, [this](const QModelIndex& topLeft, const QModelIndex& bottomRight, const QVector<int>& roles) {
        (void)bottomRight;
        if (roles.contains(Qt::CheckStateRole) && topLeft.column() == ModListModel::Enabled) {
            auto id = mod_model_->data(topLeft.sibling(topLeft.row(), ModListModel::Name), Qt::EditRole).toString();
            bool enabled = mod_model_->data(topLeft, Qt::CheckStateRole).toInt() == Qt::Checked;
            sync_mod_enable_state(id, enabled);

            // Update status bar mod count
            int count = 0;
            for (const auto& m : mod_model_->mods()) {
                if (!m.is_separator && !m.is_overwrite && m.enabled) ++count;
            }
            status_bar_->set_counter_value(count);
        }
    });

    // Sync priority rewrites to metadata files after reorder
    connect(mod_model_, &ModListModel::mod_list_changed, this, &MainWindow::sync_priorities);

    // Save order on every model change
    connect(mod_model_, &ModListModel::mod_list_changed, this, [this]() {
        if (!loading_) {
            //engine::Logger::instance().debug("Saving order on mod_list_changed");
            save_order();
            sync_separator_ids();
        } else {
            //engine::Logger::instance().debug("Suppressed order save (loading)");
        }
    });

    // Fold/unfold on separator arrow click (arrow is in the Enabled column)
    connect(mod_view_, &QTreeView::clicked, this, [this](const QModelIndex& idx) {
        if (!idx.isValid() || idx.column() != ModListModel::Enabled) return;
        int row = idx.row();
        if (row < 0 || row >= mod_model_->mods().size()) return;
        if (mod_model_->mods()[row].is_separator) {
            mod_model_->set_folded(row, !mod_model_->mods()[row].folded);
        }
    });

    // Drag-and-drop archives onto the mod list to install manually
    connect(mod_view_, &ModTableView::files_dropped, this, [this](const QStringList& paths) {
        if (current_instance_root_.empty()) return;
        auto dl_dir = current_instance_root_ / "downloads";
        std::error_code ec;
        std::filesystem::create_directories(dl_dir, ec);

        for (const auto& path : paths) {
            QFileInfo fi(path);
            auto dest = dl_dir / fi.fileName().toStdString();

            // Copy archive to instance downloads folder
            if (!QFile::exists(QString::fromStdString(dest.string()))) {
                if (!QFile::copy(path, QString::fromStdString(dest.string()))) {
                    engine::Logger::instance().error(
                        "Failed to copy archive to downloads: " + dest.string());
                    continue;
                }
            }

            auto mod_id = fi.completeBaseName().toStdString();

            // Show in DownloadsTab immediately with file path, mark ready
            auto* dt = right_panel_->downloads_tab();
            if (dt) {
                dt->add_download(mod_id, mod_id, "Manual", dest);
                dt->mark_complete(mod_id, true);
            }
        }
    });

    auto* mod_header = new ColumnToggleHeaderView(Qt::Horizontal, mod_view_);
    mod_header->set_column_labels({"Enabled", "Name", "Version", "Flags", "Priority"});
    mod_view_->setHeader(mod_header);

    mod_header->setStretchLastSection(false);
    mod_header->setSectionsMovable(true);
    mod_header->setSectionResizeMode(ModListModel::Enabled, QHeaderView::ResizeToContents);
    mod_header->setSectionResizeMode(ModListModel::Name, QHeaderView::Stretch);
    mod_header->setSectionResizeMode(ModListModel::Version, QHeaderView::Interactive);
    mod_header->setSectionResizeMode(ModListModel::Flags, QHeaderView::Interactive);
    mod_header->setSectionResizeMode(ModListModel::Priority, QHeaderView::Interactive);
    mod_header->resizeSection(ModListModel::Version, 80);
    mod_header->resizeSection(ModListModel::Flags, 80);
    mod_header->resizeSection(ModListModel::Priority, 60);

    left_layout->addWidget(mod_view_, 1);

    filter_bar_ = new ModFilterBar(this);
    left_layout->addWidget(filter_bar_);

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
    create_game_lock_overlay();

    // --- Status bar ---
    status_bar_ = new GmmStatusBar(this);
    statusBar()->addWidget(status_bar_, 1);

    // Global event filter for Konami code (child widgets may eat arrow keys)
    QApplication::instance()->installEventFilter(this);
    setFocusPolicy(Qt::StrongFocus);

    pipeline_thread_ = new PipelineThread(this);
    pipeline_thread_->start();

    // Reload mod list when a pipeline operation finishes
    connect(pipeline_thread_->worker(), &PipelineWorker::finished,
            this, [this](const std::string& mod_id, bool success, const std::string&) {
        if (success && !current_game_id_.empty()) {
            engine::Logger::instance().debug(
                "Pipeline finished for " + mod_id + ", reloading mods...");
            load_mods_from_game();
            // Mark installed in DownloadsTab (pipeline includes install + deploy)
            auto* dt = right_panel_->downloads_tab();
            if (dt) dt->mark_installed(mod_id);
        } else if (!success) {
            auto* dt = right_panel_->downloads_tab();
            if (dt) dt->mark_complete(mod_id, false);
        }
        // Persist download state
        save_download_manifest();
    });

    // Forward download progress to the DownloadsTab
    connect(pipeline_thread_->worker(), &PipelineWorker::download_progress,
            this, [this](const std::string& mod_id, int64_t dl, int64_t total, double speed) {
        auto* dt = right_panel_->downloads_tab();
        if (dt) dt->update_progress(mod_id, dl, total, speed);
    });

    // Register built-in source providers
    engine::SourceRegistry::instance().register_provider(
        std::make_unique<engine::NexusProvider>());

    auto home = std::getenv("HOME");
    std::string ws_db = home
        ? (std::string(home) + "/.local/share/GameModManager/workshop_cache.db")
        : "workshop_cache.db";
    engine::SourceRegistry::instance().register_provider(
        std::make_unique<engine::SteamWorkshopProvider>(ws_db));

    connect(right_panel_->exec_controls(), &ExecControlsBar::run_clicked, this, &MainWindow::launch_game);

    connect(right_panel_->exec_controls(), &ExecControlsBar::shortcut_to_toolbar,
            this, &MainWindow::add_shortcut_to_toolbar);

    connect(right_panel_->exec_controls(), &ExecControlsBar::shortcut_to_desktop,
            this, &MainWindow::add_shortcut_to_desktop);

    connect(right_panel_->exec_controls(), &ExecControlsBar::select_executable_requested,
            this, &MainWindow::pick_executable_file);

    // Start IPC server to receive nxm:// URLs from other GMM processes
    nxm_ipc_ = new engine::NxmIpcServer(this);
    if (nxm_ipc_->startListening()) {
        connect(nxm_ipc_, &engine::NxmIpcServer::nxmUrlReceived,
                this, [this](const QString& url) {
            std::string raw = url.toStdString();
            // Accept gmm:// URLs too — convert to nxm:// for the parser
            static const std::string gmm_pre = "gmm://nexus/";
            if (raw.compare(0, gmm_pre.size(), gmm_pre) == 0)
                raw = "nxm://" + raw.substr(gmm_pre.size());
            auto link = engine::NxmRouter::parse(raw);
            if (link.valid()) {
                handle_nxm_download(link);
            }
        });
    }

    connect_menu_actions();
    setup_mod_list_context_menu();
}

void MainWindow::on_notification(const QString& title, const QString& message) {
    status_bar_->set_status(title + ": " + message);
}

void MainWindow::set_game_info(const std::string& game_id,
                                const std::string& game_display_name,
                                const std::string& profile_name,
                                const std::filesystem::path& game_dir,
                                const std::filesystem::path& instance_root) {
    // Clear previous instance state before switching
    console_->clear();
    toolbar_->clear_exec_buttons();
    toolbar_shortcut_paths_.clear();
    right_panel_->exec_controls()->clear_executables();

    current_game_id_ = game_id;
    current_game_name_ = game_display_name;
    current_profile_name_ = profile_name;
    current_game_dir_ = game_dir;
    current_instance_root_ = instance_root;
    if (!instance_root.empty())
        conflict_cache_path_ = instance_root / "cache" / "conflict_cache.json";
    update_title();

    // Log loaded plugins now that console callback is registered
    if (plugin_loader_ && !plugin_loader_->plugins().empty()) {
        std::string list_str;
        for (size_t i = 0; i < plugin_loader_->plugins().size(); ++i) {
            if (i > 0) list_str += ", ";
            list_str += plugin_loader_->plugins()[i].game_display_name;
        }
        engine::Logger::instance().debug("Loaded plugins: [" + list_str + "]");
    }

    if (!game_dir.empty() && knowledge_) {
        update_status_bar_for_game();

        // Rebuild right-panel tabs for this game
        if (plugin_loader_)
            right_panel_->set_capabilities(&plugin_loader_->capabilities());
        right_panel_->set_game(current_game_id_);

        load_mods_from_game();
        load_executables();
        populate_executables();

        // Check if a sort provider is available for this game
        bool has_sort_provider = engine::SortRegistry::instance().get_provider(game_id) != nullptr;
        menu_bar_->set_sort_available(has_sort_provider);

        // Configure pipeline for this game instance
        engine::PipelineContext ctx;
        ctx.game_dir = current_game_dir_;
        ctx.meta_dir = current_instance_root_ / "meta";
        ctx.mods_dir = mods_dir_path();

        // Set up deploy strategy
        ctx.deploy_prefix = knowledge_->get(current_game_id_, "deploy_prefix", "Data");
        auto inc_id = knowledge_->get(current_game_id_, "deploy_include_mod_id", "false");
        ctx.deploy_include_mod_id = (inc_id == "true");
        std::unique_ptr<engine::DeploymentStrategy> deploy_strategy;
#ifdef GMM_PLATFORM_LINUX
        if (engine::OverlayFsLauncher::is_supported(current_instance_root_ / "overwrite")) {
            // OverlayFS: deploy symlinks into staging dir (not game_dir)
            auto staging = current_instance_root_ / ".gmm_staging";
            ctx.staging_dir = staging;
            auto ovl_strat = std::make_unique<engine::OverlayFsDeployStrategy>(staging);
            staging_dir_ = staging;
            deploy_strategy = std::move(ovl_strat);
            engine::Logger::instance().info("Deploy strategy: OverlayFS");
        } else
#endif
        {
            deploy_strategy = std::make_unique<engine::SymlinkStrategy>();
            engine::Logger::instance().info("Deploy strategy: Symlink (direct to game_dir)");
        }
        ctx.deploy_strategy = deploy_strategy.get();

        auto pipeline = std::make_unique<engine::Pipeline>();
        pipeline->set_context(ctx);
        pipeline->add_stage(std::make_unique<engine::FetchStage>());
        pipeline->add_stage(std::make_unique<engine::ExtractStage>());
        pipeline->add_stage(std::make_unique<engine::InstallStage>());
        pipeline->add_stage(std::make_unique<engine::DeployStage>());
        pipeline_thread_->worker()->set_pipeline(std::move(pipeline));
        pipeline_thread_->worker()->set_context(ctx);

        // Keep strategy alive for the lifetime of this session
        deploy_strategy_ = std::move(deploy_strategy);

        // Populate Tools menu with game-specific tools
        if (plugin_loader_) {
            menu_bar_->update_tools_for_game(current_game_id_,
                plugin_loader_->tool_registry().tools_for_game(current_game_id_));
        }

        // Prompt to register this game for nxm:// handling if not already managed
        prompt_nxm_registration();
    }

    // Restore saved app state now that current_instance_root_ is known
    restore_app_state();

    // Sync process tree checkbox/tree with restored state (overlay was created before restore)
    if (process_tree_checkbox_)
        process_tree_checkbox_->setChecked(show_process_tree_);
    if (process_tree_)
        process_tree_->setVisible(show_process_tree_);

    // Load persisted download entries
    load_download_manifest();

    // Connect download double-click to install (tab exists after set_game above)
    auto* dt = right_panel_->downloads_tab();
    if (dt) {
        connect(dt, &DownloadsTab::install_requested,
                this, [this](const std::string& mod_id, const std::filesystem::path& fp) {
            if (!pipeline_thread_) return;
            QMetaObject::invokeMethod(pipeline_thread_->worker(), [this, mod_id, fp]() {
                pipeline_thread_->worker()->install_mod(mod_id, fp.string());
            }, Qt::QueuedConnection);
        });
    }

    // Show debug window if debugging.enabled flag exists
    if (!current_instance_root_.empty()) {
        auto flag = current_instance_root_ / "debugging.enabled";
        if (std::filesystem::exists(flag)) {
            if (!debug_window_) {
                debug_window_ = new DebugWindow(current_instance_root_,
                    current_game_id_, current_game_name_,
                    plugin_loader_,
                    [this]() { if (style_manager_) style_manager_->reload_current(); }, this);
            }
            debug_window_->show();
            debug_window_->raise();
        } else if (debug_window_) {
            debug_window_->hide();
        }
    }
}

void MainWindow::update_title() {
    if (current_game_name_.empty()) {
        setWindowTitle("GameModManager");
    } else if (current_profile_name_.empty()) {
        setWindowTitle(("GameModManager - " + current_game_name_).c_str());
    } else {
        setWindowTitle(("GameModManager - " + current_profile_name_ + " - " + current_game_name_).c_str());
    }
}

void MainWindow::update_status_bar_for_game() {
    if (!knowledge_ || current_game_id_.empty()) return;

    // Counter label: "Mods" for folder-based games, "Plugins" for ESM/ESP games
    auto counter_label = knowledge_->get(current_game_id_, "mod_counter_label", "Mods");
    status_bar_->set_counter_label(QString::fromStdString(counter_label));

    // Download sources: comma-separated list (e.g. "Nexus,Steam")
    auto sources_csv = knowledge_->get(current_game_id_, "download_sources", "");
    QStringList sources;
    if (!sources_csv.empty()) {
        for (const auto& part : QString::fromStdString(sources_csv).split(',', Qt::SkipEmptyParts)) {
            sources.append(part.trimmed());
        }
    }
    status_bar_->set_sources(sources);
}

void MainWindow::setup_menu_bar() {
    menu_bar_ = new AppMenuBar(this);
    menu_bar_->setContextMenuPolicy(Qt::PreventContextMenu);
    setMenuWidget(menu_bar_);
}

void MainWindow::connect_menu_actions() {
    // --- File ---
    connect(menu_bar_, &AppMenuBar::new_instance_requested, this, [this]() {
        QMessageBox::information(this, "New Instance", "New Instance - coming soon");
    });
    connect(menu_bar_, &AppMenuBar::open_instance_requested, this, [this]() {
        QMessageBox::information(this, "Open Instance", "Open Instance - coming soon");
    });
    connect(menu_bar_, &AppMenuBar::recent_instance_selected, this, [this](const QString& name) {
        engine::Logger::instance().debug("Opening recent instance: " + name.toStdString());
    });
    connect(menu_bar_, &AppMenuBar::import_mods_requested, this, [this]() {
        QMessageBox::information(this, "Import Mods", "Import Mods - coming soon");
    });
    connect(menu_bar_, &AppMenuBar::export_mods_requested, this, [this]() {
        QMessageBox::information(this, "Export Mods", "Export Mods - coming soon");
    });
    connect(menu_bar_, &AppMenuBar::settings_requested, this, &MainWindow::show_settings_dialog);
    connect(menu_bar_, &AppMenuBar::exit_requested, this, [this]() {
        QApplication::quit();
    });

    // --- Edit ---
    connect(menu_bar_, &AppMenuBar::select_all_requested, this, [this]() {
        mod_view_->selectAll();
    });
    connect(menu_bar_, &AppMenuBar::deselect_all_requested, this, [this]() {
        mod_view_->clearSelection();
    });
    connect(menu_bar_, &AppMenuBar::enable_selected_requested, this, [this]() {
        auto sel = mod_view_->selectionModel()->selectedRows();
        for (const auto& idx : sel) {
            mod_model_->setData(idx, Qt::Checked, Qt::CheckStateRole);
        }
        engine::Logger::instance().debug("Enabled " + std::to_string(sel.size()) + " mods");
    });
    connect(menu_bar_, &AppMenuBar::disable_selected_requested, this, [this]() {
        auto sel = mod_view_->selectionModel()->selectedRows();
        for (const auto& idx : sel) {
            mod_model_->setData(idx, Qt::Unchecked, Qt::CheckStateRole);
        }
        engine::Logger::instance().debug("Disabled " + std::to_string(sel.size()) + " mods");
    });
    connect(menu_bar_, &AppMenuBar::priority_up_requested, this, [this]() {
        engine::Logger::instance().debug("Priority Up requested");
    });
    connect(menu_bar_, &AppMenuBar::priority_down_requested, this, [this]() {
        engine::Logger::instance().debug("Priority Down requested");
    });

    // --- View ---
    connect(menu_bar_, &AppMenuBar::toggle_toolbar, this, [this](bool visible) {
        toolbar_area_->setVisible(visible);
    });
    connect(menu_bar_, &AppMenuBar::toggle_status_bar, this, [this](bool visible) {
        statusBar()->setVisible(visible);
    });
    connect(menu_bar_, &AppMenuBar::toggle_console, this, [this](bool visible) {
        if (visible) {
            console_splitter_->setSizes({700, 200});
        } else {
            console_splitter_->setSizes({700, 0});
        }
    });
    connect(menu_bar_, &AppMenuBar::refresh_requested, this, [this]() {
        engine::Logger::instance().debug("Refresh requested");
    });

    // Populate Columns submenu from the table header
    auto* header = mod_view_->header();
    auto* cols_menu = menu_bar_->findChild<QMenu*>("Columns", Qt::FindDirectChildrenOnly);
    if (!cols_menu) {
        // Find columns menu by iterating menus
        for (auto* m : menu_bar_->actions()) {
            if (m->menu() && m->text() == "&View") {
                for (auto* sub : m->menu()->actions()) {
                    if (sub->menu() && sub->text() == "Columns") {
                        cols_menu = sub->menu();
                        break;
                    }
                }
                break;
            }
        }
    }
    if (cols_menu) {
        for (int i = 0; i < header->count(); ++i) {
            auto* act = cols_menu->addAction(header->model()->headerData(i, Qt::Horizontal).toString());
            act->setCheckable(true);
            act->setChecked(!header->isSectionHidden(i));
            connect(act, &QAction::toggled, this, [header, i](bool show) {
                header->setSectionHidden(i, !show);
            });
        }
    }

    // --- Tools ---
    connect(menu_bar_, &AppMenuBar::tool_requested, this, [this](const QString& tool_id, const QString& game_id) {
        if (!plugin_loader_) return;
        auto* tool = plugin_loader_->tool_registry().get_tool(game_id.toStdString(),
                                                               tool_id.toStdString());
        if (!tool) {
            engine::Logger::instance().warn("Tool not found: " + tool_id.toStdString() +
                                            " for game: " + game_id.toStdString());
            return;
        }
        engine::Logger::instance().debug("Running tool: " + tool_id.toStdString() +
                                        " for game: " + game_id.toStdString());
        if (tool->invoke_fn) {
            tool->invoke_fn(tool->invoke_user_data);
        } else {
            // Fallback: try launching the executable
            for (const auto& path : tool->search_paths) {
                auto exe_path = std::filesystem::path(path) / tool->executable_name;
                std::error_code ec;
                if (std::filesystem::exists(exe_path, ec)) {
                    QProcess::startDetached(QString::fromStdString(exe_path.string()),
                                            QStringList());
                    return;
                }
            }
            engine::Logger::instance().warn("Tool executable not found: " +
                                            tool->executable_name);
        }
    });
    connect(menu_bar_, &AppMenuBar::open_instance_folder_requested, this, [this]() {
        if (current_instance_root_.empty()) return;
        auto url = QUrl::fromLocalFile(QString::fromStdString(current_instance_root_.string()));
        QDesktopServices::openUrl(url);
    });
    connect(menu_bar_, &AppMenuBar::open_mods_folder_requested, this, [this]() {
        if (current_game_dir_.empty()) return;
        auto path = mods_dir_path();
        QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(path.string())));
    });
    connect(menu_bar_, &AppMenuBar::open_downloads_folder_requested, this, [this]() {
        if (current_instance_root_.empty()) return;
        auto dl_dir = current_instance_root_ / "downloads";
        std::error_code ec;
        if (!std::filesystem::exists(dl_dir, ec))
            std::filesystem::create_directories(dl_dir, ec);
        QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(dl_dir.string())));
    });

    // --- Help ---
    connect(menu_bar_, &AppMenuBar::about_requested, this, [this]() {
        QMessageBox::about(this, "About GameModManager",
            "<h3>GameModManager</h3>"
            "<p>Version " VERSION "</p>"
            "<p>Cross-platform game mod manager with multi-repo plugin support.</p>");
    });
    connect(menu_bar_, &AppMenuBar::about_qt_requested, this, [this]() {
        QMessageBox::aboutQt(this, "About Qt");
    });
    connect(menu_bar_, &AppMenuBar::check_updates_requested, this, [this]() {
        QMessageBox::information(this, "Updates", "You are running the latest version.");
    });
    connect(menu_bar_, &AppMenuBar::instance_statistics_requested, this, &MainWindow::show_instance_statistics);
}

void MainWindow::sync_mod_enable_state(const QString& mod_id, bool enabled) {
    if (loading_) return;
    if (!knowledge_ || current_game_id_.empty() || current_game_dir_.empty()) return;

    // Separators don't have enable/disable on disk
    for (const auto& m : mod_model_->mods()) {
        if (m.id == mod_id && m.is_separator) return;
    }

    // Game is running — queue the change instead of writing to disk
    if (running_process_pid_ > 0) {
        // Remove any existing pending toggle for this mod (latest wins)
        auto it = std::remove_if(pending_changes_.begin(), pending_changes_.end(),
            [&](const PendingToggle& pt) { return pt.mod_id == mod_id; });
        pending_changes_.erase(it, pending_changes_.end());
        pending_changes_.push_back({mod_id, enabled});
        update_queue_label();
        engine::Logger::instance().debug("Queued toggle for " + mod_id.toStdString() +
            " -> " + (enabled ? "enabled" : "disabled") + " (" +
            std::to_string(pending_changes_.size()) + " pending)");
        return;
    }

    auto mods_subpath = knowledge_->get(current_game_id_, "mods_subpath", "");
    if (mods_subpath.empty()) return;

    auto mod_folder = mods_dir_path() / mod_id.toStdString();

    if (enabled) {
        (void)engine::ModScanner::enable_mod(*knowledge_, current_game_id_, mod_folder);
    } else {
        (void)engine::ModScanner::disable_mod(*knowledge_, current_game_id_, mod_folder);
    }
}

void MainWindow::sync_priorities() {
    if (loading_) return;
    if (!knowledge_ || current_game_id_.empty() || current_game_dir_.empty()) return;

    // Game is running — skip disk write; full order saved at flush
    if (running_process_pid_ > 0) {
        return;
    }

    auto meta_dir = meta_dir_path();
    auto mods_subpath = knowledge_->get(current_game_id_, "mods_subpath", "");

    auto& mods = mod_model_->mods();
    for (int i = 0; i < mods.size(); ++i) {
        // Persist priority to meta.ini for every row (Overwrite, separators, mods)
        if (!meta_dir.empty()) {
            auto meta = engine::ModMeta::load(meta_dir, mods[i].id.toStdString());
            if (meta.priority() != i) {
                meta.set_priority(i);
                meta.save(meta_dir, mods[i].id.toStdString());
            }
        }
        // Write game-native priority only for real mods
        if (!mods[i].is_overwrite && !mods[i].is_separator && !mods_subpath.empty()) {
            auto mod_folder = mods_dir_path() / mods[i].id.toStdString();
            (void)engine::ModScanner::set_priority(*knowledge_, current_game_id_, mod_folder, i);
        }
    }
}

void MainWindow::sort_mods() {
    auto* provider = engine::SortRegistry::instance().get_provider(current_game_id_);
    if (!provider) {
        engine::Logger::instance().warn("No sort provider registered for game: " + current_game_id_);
        return;
    }

    // Build mod info list from current model
    std::vector<engine::SortModInfo> mod_infos;
    for (const auto& mod : mod_model_->mods()) {
        if (mod.is_separator || mod.is_overwrite || mod.id == kOverwriteModId) continue;

        engine::SortModInfo info;
        info.folder_name = mod.id.toStdString();
        info.display_name = mod.name.toStdString();

        // Extract workshop ID from folder name
        auto workshop_pattern = knowledge_->get(current_game_id_, "workshop_id_pattern", "");
        if (!workshop_pattern.empty()) {
            try {
                std::regex re(workshop_pattern);
                std::smatch m;
                if (std::regex_search(info.folder_name, m, re)) {
                    info.workshop_id = std::stoll(m[1].str());
                }
            } catch (...) {}
        }

        mod_infos.push_back(info);
    }

    // Call the sort provider
    auto result = provider->sort(mod_infos);

    // Apply the sorted order to the model
    loading_ = true;

    // Build a map of folder_name -> ModEntry
    QMap<QString, ui::ModEntry> mod_map;
    for (const auto& mod : mod_model_->mods()) {
        mod_map[mod.id] = mod;
    }

    // Create new ordered list
    QVector<ui::ModEntry> new_order;

    // Add mods in sorted order
    for (const auto& folder : result.sorted_folders) {
        auto qfolder = QString::fromStdString(folder);
        if (mod_map.contains(qfolder)) {
            new_order.append(mod_map[qfolder]);
        }
    }

    // Add any mods not in the sorted result (shouldn't happen, but be safe)
    for (const auto& mod : mod_model_->mods()) {
        if (!mod.is_overwrite && std::find(result.sorted_folders.begin(), result.sorted_folders.end(), mod.id.toStdString()) == result.sorted_folders.end()) {
            new_order.append(mod);
        }
    }

    // Overwrite always at bottom
    for (const auto& mod : mod_model_->mods()) {
        if (mod.is_overwrite) {
            new_order.append(mod);
            break;
        }
    }

    // Apply the new order
    mod_model_->reset_with_order(new_order);

    // Apply tags from sort result
    for (const auto& tag_info : result.tags) {
        QVector<ui::ModTag> tags;
        tags.append({QString::fromStdString(tag_info.type), QString::fromStdString(tag_info.message)});
        mod_model_->set_tags(QString::fromStdString(tag_info.folder_name), tags);
    }

    loading_ = false;
    save_order();

    engine::Logger::instance().debug("Mods sorted by " + std::string(provider->name()));
}

void MainWindow::load_mods_from_game() {
    if (!knowledge_ || current_game_id_.empty() || current_game_dir_.empty()) return;

    loading_ = true;

    // Configure conflict order from plugin hook (before adding mods)
    auto conflict_reversed = knowledge_->get(current_game_id_, "conflict_order_reversed", "");
    mod_model_->set_conflict_order_reversed(conflict_reversed == "true");

    // Scan game's native mods directory
    auto scanned = engine::ModScanner::scan(*knowledge_, current_game_id_, current_game_dir_,
        current_instance_root_.empty() ? std::vector<std::filesystem::path>{}
                                       : std::vector<std::filesystem::path>{current_instance_root_});

    // Also scan instance mods directory if different from game mods dir
    if (!current_instance_root_.empty()) {
        auto instance_mods_dir = mods_dir_path();
        auto game_mods_subpath = knowledge_->get(current_game_id_, "mods_subpath", "");
        auto game_mods_dir = current_game_dir_ / game_mods_subpath;
        // Only scan separately if they're different directories
        std::error_code ec_canon;
        auto inst_canon = std::filesystem::weakly_canonical(instance_mods_dir, ec_canon);
        auto game_canon = std::filesystem::weakly_canonical(game_mods_dir, ec_canon);
        if (inst_canon != game_canon) {
            auto instance_scanned = engine::ModScanner::scan_dir(
                *knowledge_, current_game_id_, instance_mods_dir,
                std::vector<std::filesystem::path>{});
            // Merge — instance mods override game-native mods with same folder name
            std::unordered_set<std::string> seen;
            for (const auto& m : instance_scanned)
                seen.insert(m.folder_name);
            for (auto& m : scanned) {
                if (seen.count(m.folder_name))
                    continue;
                instance_scanned.push_back(std::move(m));
            }
            scanned = std::move(instance_scanned);
        }
    }

    // Detect game-native plugins (e.g. vanilla ESMs) from the game's mods directory
    {
        auto native_plugins_csv = knowledge_->get(current_game_id_, "game_native_plugins", "");
        if (!native_plugins_csv.empty()) {
            auto game_mods_subpath = knowledge_->get(current_game_id_, "mods_subpath", "");
            std::filesystem::path native_dir = current_game_dir_;
            if (!game_mods_subpath.empty())
                native_dir /= game_mods_subpath;

            std::unordered_set<std::string> existing;
            for (const auto& m : scanned)
                existing.insert(m.folder_name);

            std::istringstream ss(native_plugins_csv);
            std::string plugin;
            while (std::getline(ss, plugin, ',')) {
                auto start = plugin.find_first_not_of(" \t");
                auto end = plugin.find_last_not_of(" \t");
                if (start == std::string::npos) continue;
                plugin = plugin.substr(start, end - start + 1);
                if (existing.count(plugin)) continue;

                auto plugin_path = native_dir / plugin;
                if (!std::filesystem::exists(plugin_path)) continue;

                engine::ScannedMod native_mod;
                native_mod.folder_name = plugin;
                native_mod.display_name = plugin;
                native_mod.raw_name = plugin;
                native_mod.is_game_native = true;
                native_mod.enabled = true;
                scanned.push_back(std::move(native_mod));
            }
        }
    }

    // Clear existing mods (except Overwrite which ensure_overwrite_present manages)
    auto existing = mod_model_->mods();
    for (int i = existing.size() - 1; i >= 0; --i) {
        if (!existing[i].is_overwrite) {
            mod_model_->remove_mod(existing[i].id);
        }
    }

    // Add scanned mods before Overwrite (Overwrite stays last)
    for (const auto& mod : scanned) {
        auto id = QString::fromStdString(mod.folder_name);
        auto name = QString::fromStdString(mod.display_name);
        auto ver = QString::fromStdString(mod.version);
        if (mod.is_separator) {
            auto color = QString::fromStdString(mod.separator_color);
            mod_model_->add_separator(id, name, color);
        } else {
            mod_model_->add_mod(id, name, ver, mod.priority, mod.is_game_native);
            if (!mod.enabled) {
                mod_model_->toggle_mod(id);
            }
        }
    }

    // Import MO2 meta.ini files and load/create meta for each mod
    migrate_mo2_meta();
    load_meta_for_mods();

    // Read persisted priority from meta.ini for ALL entries (including separators, Overwrite)
    {
        auto meta_dir = meta_dir_path();
        if (!meta_dir.empty()) {
            for (const auto& m : mod_model_->mods()) {
                auto meta = engine::ModMeta::load(meta_dir, m.id.toStdString());
                int p = meta.priority();
                if (p >= 0) {
                    mod_model_->set_priority(m.id, p);
                }
            }
        }
    }

    loading_ = false;

    // Sort by priority to restore saved order
    load_order();

    // Sync separator IDs for new mods or first-load (no instance.toml yet)
    sync_separator_ids();

    // Tell the model where Overwrite lives so it can colour the entry
    if (!current_instance_root_.empty()) {
        auto overwrite_dir = current_instance_root_ / "overwrite";
        mod_model_->set_overwrite_path(QString::fromStdString(overwrite_dir.string()));
    }

    engine::Logger::instance().debug("Loaded " + std::to_string(scanned.size()) +
        " mods for " + current_game_name_);

    // Update status bar mod count
    int count = 0;
    for (const auto& m : mod_model_->mods()) {
        if (!m.is_separator && !m.is_overwrite && m.enabled) ++count;
    }
    status_bar_->set_counter_value(count);

    // Compute conflict stats for all mods
    recompute_conflicts();
}

std::filesystem::path MainWindow::meta_dir_path() const {
    if (current_instance_root_.empty()) return {};
    return current_instance_root_ / "meta";
}

std::filesystem::path MainWindow::mods_dir_path() const {
    if (current_instance_root_.empty() && current_game_dir_.empty()) return {};
    // MO2-style: mods managed from instance root (even if mods_subpath is set)
    if (!current_instance_root_.empty())
        return current_instance_root_ / "mods";
    auto subpath = knowledge_ ? knowledge_->get(current_game_id_, "mods_subpath", "") : "";
    if (!subpath.empty())
        return current_game_dir_ / subpath;
    return current_game_dir_;
}

void MainWindow::migrate_mo2_meta() {
    auto meta_dir = meta_dir_path();
    if (meta_dir.empty()) return;

    auto mods_dir = mods_dir_path();

    if (!std::filesystem::exists(mods_dir)) return;

    // Steam appid for this game (needed for Workshop mods)
    auto steam_appid_str = knowledge_->get(current_game_id_, "steam_appid", "0");

    for (const auto& entry : std::filesystem::directory_iterator(mods_dir)) {
        if (!entry.is_directory()) continue;
        auto folder_name = entry.path().filename().string();

        // Skip if meta already imported
        if (engine::ModMeta::exists(meta_dir, folder_name))
            continue;

        // Check if MO2 meta.ini exists in this mod's folder
        if (!engine::ModMeta::has_mo2_meta(entry.path()))
            continue;

        // Import
        auto meta = engine::ModMeta::import_mo2(entry.path(), folder_name);
        if (!meta.has_section("General") && !meta.has_section("GameModManager"))
            continue;

        // Fill in the steam_appid from game knowledge
        if (!steam_appid_str.empty() && steam_appid_str != "0") {
            meta.set("GameModManager", "steam_appid", steam_appid_str);
        }

        if (meta.save(meta_dir, folder_name)) {
            engine::Logger::instance().debug("Imported MO2 meta: " + folder_name + "/meta.ini");
        } else {
            engine::Logger::instance().warn("Failed to save imported meta: " + folder_name);
        }
    }
}

void MainWindow::load_meta_for_mods() {
    auto meta_dir = meta_dir_path();
    if (meta_dir.empty()) return;

    // Workshop ID pattern — used to detect Steam Workshop mods from folder names
    auto workshop_pattern = knowledge_->get(current_game_id_, "workshop_id_pattern", "");

    auto mods = mod_model_->mods();
    for (int i = 0; i < mods.size(); ++i) {
        const auto& mod = mods[i];
        if (mod.is_separator || mod.is_overwrite) continue;

        auto folder_name = mod.id.toStdString();
        if (folder_name.empty()) continue;

        // Load existing meta (or empty if no file yet)
        auto meta = engine::ModMeta::load(meta_dir, folder_name);

        if (!meta.has_section("General") && !meta.has_section("GameModManager")) {
            // No meta file exists — create a default one (already at CURRENT_META_VERSION)
            meta = engine::ModMeta::from_default(folder_name, "manual", "");
            meta.save(meta_dir, folder_name);

        } else {
            // Existing meta — check if upgrade is needed
            bool upgraded = false;
            int mv = meta.meta_version();

            if (mv < engine::ModMeta::CURRENT_META_VERSION) {
                // v0 → v1: detect Steam Workshop mods and write [SteamWorkshop]workshop_id
                if (mv < 1 && !workshop_pattern.empty()) {
                    try {
                        std::regex pattern(workshop_pattern);
                        std::smatch m;
                        if (std::regex_search(folder_name, m, pattern) && m.size() > 1) {
                            if (meta.get("SteamWorkshop", "workshop_id").empty()) {
                                meta.set("SteamWorkshop", "workshop_id", m[1].str());
                            }
                        }
                    } catch (...) {}
                }

                // Future v1 → v2 upgrades go here

                meta.set_meta_version(engine::ModMeta::CURRENT_META_VERSION);
                upgraded = true;
            }

            if (upgraded) {
                meta.save(meta_dir, folder_name);
            }
        }

        // Update ModEntry with source info
        auto st = meta.source_type();
        auto sid = meta.source_id();
        if (!st.empty()) {
            mod_model_->set_source_info(mod.id, QString::fromStdString(st),
                                        QString::fromStdString(sid));
        }

        // Update ModEntry with separator info from meta.ini
        auto sep_id = meta.separator_id();
        if (!sep_id.empty()) {
            mod_model_->set_separator_id(mod.id, QString::fromStdString(sep_id));
        }
    }
}

void MainWindow::recompute_conflicts() {
    if (!knowledge_ || current_game_id_.empty() || current_game_dir_.empty())
        return;

    // Read per-game config from knowledge hooks (needed before mod_infos for overwrite priority)
    auto conflict_extensions = knowledge_->get(current_game_id_, "conflict_extensions", "");
    auto ignored_files = knowledge_->get(current_game_id_, "ignored_files", "");
    auto conflict_reversed = knowledge_->get(current_game_id_, "conflict_order_reversed", "") == "true";

    // Collect mod info from the current model
    std::vector<engine::ConflictEngine::ModInfo> mod_infos;
    for (const auto& mod : mod_model_->mods()) {
        if (mod.is_separator) continue;
        if (mod.is_overwrite) {
            // Overwrite always wins: lowest priority for Isaac, highest for everyone else
            int ow_priority = conflict_reversed ? -1 : 999999;
            mod_infos.emplace_back(mod.id.toStdString(), ow_priority);
            continue;
        }
        mod_infos.emplace_back(mod.id.toStdString(), mod.priority);
    }

    if (mod_infos.empty()) return;

    auto mods_dir = mods_dir_path();

    //engine::Logger::instance().debug(
    //    "ConflictEngine: reversed=" + std::string(conflict_reversed ? "true" : "false") +
    //    ", mods=" + std::to_string(mod_infos.size()) +
    //    ", game=" + current_game_id_);

    engine::ConflictEngine engine;
    auto stats = engine.compute(mods_dir, mod_infos,
                                 conflict_extensions, ignored_files,
                                 conflict_reversed, conflict_cache_path_);

    // Push results into the model
    for (const auto& [folder_name, cs] : stats) {
        mod_model_->set_conflict_stats(QString::fromStdString(folder_name), cs.wins, cs.losses);
        //engine::Logger::instance().debug(
        //    "ConflictStats: " + folder_name +
        //    " wins=" + std::to_string(cs.wins) +
        //    " losses=" + std::to_string(cs.losses));
    }

    // Build pairwise data from the file registry
    QMap<QString, ui::ConflictPairs> pairs;
    auto add_win = [&](const QString& winner, const QString& loser) {
        auto& w = pairs[winner];
        if (!w.wins_against.contains(loser)) w.wins_against.append(loser);
    };
    auto add_loss = [&](const QString& loser, const QString& winner) {
        auto& l = pairs[loser];
        if (!l.loses_to.contains(winner)) l.loses_to.append(winner);
    };
    for (const auto& [path, owners] : engine.last_registry()) {
        if (owners.size() <= 1) continue;
        auto winner_it = conflict_reversed
            ? std::min_element(owners.begin(), owners.end(),
                               [](const auto& a, const auto& b) { return a.second < b.second; })
            : std::max_element(owners.begin(), owners.end(),
                               [](const auto& a, const auto& b) { return a.second < b.second; });
        const auto& winner = *winner_it;
        auto wq = QString::fromStdString(winner.first);
        for (const auto& [loser_name, _] : owners) {
            if (loser_name == winner.first) continue;
            auto lq = QString::fromStdString(loser_name);
            add_win(wq, lq);
            add_loss(lq, wq);
        }
    }
    mod_model_->set_conflict_pairs(pairs);
}

void MainWindow::setup_mod_list_context_menu() {
    mod_context_menu_ = new QMenu(this);
    mod_view_->setContextMenuPolicy(Qt::CustomContextMenu);

    auto* clear_action = mod_context_menu_->addAction("Clear Overwrite");
    auto* create_mod_action = mod_context_menu_->addAction("Create Mod from Overwrite");
    mod_context_menu_->addSeparator();
    auto* edit_separator_action = mod_context_menu_->addAction("Edit Separator");
    auto* delete_separator_action = mod_context_menu_->addAction("Delete Separator");
    mod_context_menu_->addSeparator();
    auto* remove_action = mod_context_menu_->addAction("Remove");
    auto* open_folder_action = mod_context_menu_->addAction("Open in File Manager");

    connect(mod_view_, &QWidget::customContextMenuRequested,
            this, [this, clear_action, create_mod_action, edit_separator_action, delete_separator_action, remove_action, open_folder_action](const QPoint& pos) {
        auto idx = mod_view_->indexAt(pos);
        if (!idx.isValid()) return;

        int row = idx.row();
        bool is_ow = mod_model_->is_overwrite(row);
        bool is_sep = row >= 0 && row < mod_model_->mods().size() && mod_model_->mods()[row].is_separator;
        bool is_native = row >= 0 && row < mod_model_->mods().size() && mod_model_->mods()[row].is_game_native;

        clear_action->setVisible(is_ow);
        create_mod_action->setVisible(is_ow);
        edit_separator_action->setVisible(is_sep);
        delete_separator_action->setVisible(is_sep);
        remove_action->setVisible(!is_ow && !is_sep && !is_native);
        open_folder_action->setVisible(!is_sep && !is_native);

        mod_context_menu_->exec(mod_view_->viewport()->mapToGlobal(pos));
    });

    connect(clear_action, &QAction::triggered, this, [this]() {
        auto sel = mod_view_->selectionModel()->selectedRows();
        if (sel.isEmpty()) return;

        auto reply = QMessageBox::question(this, "Clear Overwrite",
            "Remove all files from the Overwrite folder? This cannot be undone.",
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (reply != QMessageBox::Yes) return;

        if (!current_instance_root_.empty()) {
            auto overwrite_dir = current_instance_root_ / "overwrite";
            if (engine::SyncStage::clear_overwrite(overwrite_dir)) {
                engine::Logger::instance().debug("Overwrite cleared");
                QMessageBox::information(this, "Overwrite", "Overwrite folder cleared.");
            } else {
                QMessageBox::warning(this, "Overwrite", "Failed to clear Overwrite folder.");
            }
        }
    });

    connect(create_mod_action, &QAction::triggered, this, [this]() {
        bool ok;
        auto name = QInputDialog::getText(this, "Create Mod from Overwrite",
            "Mod name:", QLineEdit::Normal, QString(), &ok);
        if (!ok || name.isEmpty()) return;

        if (current_instance_root_.empty()) return;

        auto overwrite_dir = current_instance_root_ / "overwrite";
        auto mods_subpath = knowledge_ ? knowledge_->get(current_game_id_, "mods_subpath", "") : std::string();
        if (mods_subpath.empty()) return;

        auto mod_dir = mods_dir_path() / name.toStdString();

        // Collect all files in Overwrite
        std::vector<std::string> rel_paths;
        std::error_code ec;
        if (std::filesystem::exists(overwrite_dir)) {
            for (const auto& entry :
                 std::filesystem::recursive_directory_iterator(overwrite_dir)) {
                if (entry.is_regular_file()) {
                    auto rel = std::filesystem::relative(entry.path(), overwrite_dir, ec);
                    if (!ec) rel_paths.push_back(rel.string());
                }
            }
        }

        if (rel_paths.empty()) {
            QMessageBox::information(this, "Create Mod", "Overwrite folder is empty.");
            return;
        }

        if (engine::SyncStage::promote_to_mod(overwrite_dir, mod_dir, rel_paths)) {
            auto id = name;
            mod_model_->add_mod(id, name, "");
            engine::Logger::instance().debug("Promote Overwrite to mod: " + name.toStdString());
            QMessageBox::information(this, "Create Mod",
                "Overwrite contents promoted to mod: " + name);
        } else {
            QMessageBox::warning(this, "Create Mod", "Failed to promote Overwrite files.");
        }
    });

    connect(remove_action, &QAction::triggered, this, [this]() {
        auto sel = mod_view_->selectionModel()->selectedRows();
        if (sel.isEmpty()) return;

        QStringList names;
        for (const auto& idx : sel) {
            int row = idx.row();
            if (row < 0 || row >= mod_model_->mods().size()) continue;
            if (mod_model_->mods()[row].is_overwrite) continue;
            names.append(mod_model_->mods()[row].name);
        }
        if (names.isEmpty()) return;

        auto reply = QMessageBox::question(this, "Remove Mods",
            "Permanently delete " + QString::number(names.size()) + " mod(s) from disk?\n\n" +
            names.join("\n") + "\n\nThis cannot be undone.",
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (reply != QMessageBox::Yes) return;

        auto mods_subpath = knowledge_ ? knowledge_->get(current_game_id_, "mods_subpath", "") : std::string();

        for (const auto& idx : sel) {
            int row = idx.row();
            if (row < 0 || row >= mod_model_->mods().size()) continue;
            const auto& entry = mod_model_->mods()[row];
            if (entry.is_overwrite) continue;

            // Delete mod folder from disk
            if (!mods_subpath.empty() && !current_game_dir_.empty()) {
                auto mod_folder = mods_dir_path() / entry.id.toStdString();
                std::error_code ec;
                std::filesystem::remove_all(mod_folder, ec);
                if (ec) {
                    engine::Logger::instance().error("Failed to remove mod folder: " + mod_folder.string() + ": " + ec.message());
                }
            }
            mod_model_->remove_mod(entry.id);
        }
    });

    connect(open_folder_action, &QAction::triggered, this, [this]() {
        auto sel = mod_view_->selectionModel()->selectedRows();
        if (sel.isEmpty()) return;
        int row = sel.first().row();
        if (row < 0 || row >= mod_model_->mods().size()) return;
        const auto& entry = mod_model_->mods()[row];
        auto folder = mods_dir_path() / entry.id.toStdString();
        QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(folder.string())));
    });

    connect(edit_separator_action, &QAction::triggered, this, [this]() {
        auto sel = mod_view_->selectionModel()->selectedRows();
        if (sel.isEmpty()) return;
        int row = sel.first().row();
        if (row >= 0 && row < mod_model_->mods().size() && mod_model_->mods()[row].is_separator) {
            edit_separator(row);
        }
    });

    connect(delete_separator_action, &QAction::triggered, this, [this]() {
        auto sel = mod_view_->selectionModel()->selectedRows();
        if (sel.isEmpty()) return;
        int row = sel.first().row();
        if (row >= 0 && row < mod_model_->mods().size() && mod_model_->mods()[row].is_separator) {
            delete_separator(row);
        }
    });
}

void MainWindow::create_separator() {
    if (!knowledge_ || current_game_id_.empty() || current_game_dir_.empty()) return;

    auto mods_subpath = knowledge_->get(current_game_id_, "mods_subpath", "");
    auto separator_suffix = knowledge_->get(current_game_id_, "separator_suffix", "_separator");
    if (mods_subpath.empty()) return;

    bool ok;
    auto name = QInputDialog::getText(this, "Create Separator",
        "Separator name:", QLineEdit::Normal, QString(), &ok);
    if (!ok || name.trimmed().isEmpty()) return;

    // Check for duplicate names
    auto existing = mod_model_->existing_separator_names();
    if (existing.contains(name.trimmed())) {
        QMessageBox::warning(this, "Separator", "A separator with this name already exists.");
        return;
    }

    // Pick color
    QColor initial("#888888");
    QColor color = QColorDialog::getColor(initial, this, "Separator Color");
    if (!color.isValid()) return;

    auto folder_name = name.trimmed().toStdString() + separator_suffix;
    auto sep_dir = mods_dir_path() / folder_name;

    // Create the separator folder
    std::error_code ec;
    std::filesystem::create_directories(sep_dir, ec);
    if (ec) {
        QMessageBox::warning(this, "Separator", "Failed to create separator directory.");
        return;
    }

    // Write separator.xml
    auto xml_path = sep_dir / "separator.xml";
    std::ofstream f(xml_path);
    if (!f) {
        QMessageBox::warning(this, "Separator", "Failed to write separator.xml.");
        return;
    }
    f << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    f << "<separator>\n";
    f << "  <name>" << name.trimmed().toStdString() << "</name>\n";
    f << "  <color>" << color.name().toStdString() << "</color>\n";
    f << "</separator>\n";
    f.close();

    // Add to model
    auto id = QString::fromStdString(folder_name);
    mod_model_->add_separator(id, name.trimmed(), color.name());
    engine::Logger::instance().debug("Separator created: " + name.toStdString());
}

void MainWindow::edit_separator(int row) {
    if (row < 0 || row >= mod_model_->mods().size()) return;
    const auto& mod = mod_model_->mods()[row];
    if (!mod.is_separator) return;

    if (!knowledge_ || current_game_id_.empty() || current_game_dir_.empty()) return;

    auto mods_subpath = knowledge_->get(current_game_id_, "mods_subpath", "");
    auto separator_suffix = knowledge_->get(current_game_id_, "separator_suffix", "_separator");
    if (mods_subpath.empty()) return;

    // Prompt for new name
    bool ok;
    auto new_name = QInputDialog::getText(this, "Edit Separator",
        "Separator name:", QLineEdit::Normal, mod.name, &ok);
    if (!ok || new_name.trimmed().isEmpty()) return;

    // Check for duplicate names (excluding self)
    auto existing = mod_model_->existing_separator_names();
    existing.removeAll(mod.name);
    if (existing.contains(new_name.trimmed())) {
        QMessageBox::warning(this, "Separator", "A separator with this name already exists.");
        return;
    }

    // Pick color
    QColor current_color(mod.separator_color.isEmpty() ? "#888888" : mod.separator_color);
    QColor color = QColorDialog::getColor(current_color, this, "Separator Color");
    if (!color.isValid()) return;

    auto old_folder = mod.id.toStdString();
    auto new_folder = new_name.trimmed().toStdString() + separator_suffix;

    auto mods_dir = mods_dir_path();
    auto old_path = mods_dir / old_folder;
    auto new_path = mods_dir / new_folder;

    // Rename folder on disk if name changed
    if (old_folder != new_folder) {
        std::error_code ec;
        std::filesystem::rename(old_path, new_path, ec);
        if (ec) {
            QMessageBox::warning(this, "Separator", "Failed to rename separator folder.");
            return;
        }
    }

    // Rewrite separator.xml
    auto xml_path = new_path / "separator.xml";
    std::ofstream f(xml_path);
    if (f) {
        f << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
        f << "<separator>\n";
        f << "  <name>" << new_name.trimmed().toStdString() << "</name>\n";
        f << "  <color>" << color.name().toStdString() << "</color>\n";
        f << "</separator>\n";
    }

    // Update model entry
    loading_ = true;
    mod_model_->remove_mod(mod.id);
    mod_model_->add_separator(QString::fromStdString(new_folder), new_name.trimmed(), color.name());
    loading_ = false;

    save_order();
    sync_separator_ids();

    engine::Logger::instance().debug("Separator edited: " + new_name.toStdString());
}

void MainWindow::delete_separator(int row) {
    if (row < 0 || row >= mod_model_->mods().size()) return;
    const auto& mod = mod_model_->mods()[row];
    if (!mod.is_separator) return;

    auto reply = QMessageBox::question(this, "Delete Separator",
        "Delete separator \"" + mod.name + "\" from disk?\n\nThis cannot be undone.",
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    if (!knowledge_ || current_game_id_.empty() || current_game_dir_.empty()) return;

    auto mods_subpath = knowledge_->get(current_game_id_, "mods_subpath", "");
    if (!mods_subpath.empty() && !current_game_dir_.empty()) {
        auto sep_folder = mods_dir_path() / mod.id.toStdString();
        std::error_code ec;
        std::filesystem::remove_all(sep_folder, ec);
        if (ec) {
            engine::Logger::instance().error("Failed to remove separator folder: " + sep_folder.string());
        }
    }

    mod_model_->remove_mod(mod.id);
    engine::Logger::instance().debug("Separator deleted: " + mod.name.toStdString());
}

void MainWindow::save_order() {
    if (current_instance_root_.empty()) return;

    // Save the ordered list of folder names (including separators) to instance.toml
    auto toml_path = current_instance_root_ / "instance.toml";

    // Read existing toml to preserve other fields
    std::ifstream in(toml_path);
    std::string existing;
    if (in) {
        existing.assign((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    }
    in.close();

    // Remove old mod_order / folded_separators / toolbar_shortcuts lines
    std::istringstream stream(existing);
    std::string line;
    std::string cleaned;
    while (std::getline(stream, line)) {
        auto key_pos = line.find("mod_order");
        if (key_pos != std::string::npos) continue;
        auto fold_pos = line.find("folded_separators");
        if (fold_pos != std::string::npos) continue;
        auto ts_pos = line.find("toolbar_shortcuts");
        if (ts_pos != std::string::npos) continue;
        cleaned += line + "\n";
    }

    // Folded separators
    cleaned += "folded_separators = [";
    bool first = true;
    auto& mods = mod_model_->mods();
    for (const auto& m : mods) {
        if (m.is_separator && m.folded) {
            if (!first) cleaned += ", ";
            cleaned += "\"" + m.name.toStdString() + "\"";
            first = false;
        }
    }
    cleaned += "]\n";

    // Toolbar shortcuts
    cleaned += "toolbar_shortcuts = [";
    bool first_ts = true;
    for (const auto& path : toolbar_shortcut_paths_) {
        if (!first_ts) cleaned += ", ";
        cleaned += "\"" + path.toStdString() + "\"";
        first_ts = false;
    }
    cleaned += "]\n";

    std::ofstream out(toml_path);
    if (out) out << cleaned;
}

void MainWindow::load_order() {
    if (current_instance_root_.empty()) return;

    auto toml_path = current_instance_root_ / "instance.toml";
    std::ifstream in(toml_path);
    if (!in) return;

    std::string line;
    std::vector<std::string> order;     // migrated from mod_order (for backward compat)
    std::vector<std::string> folded_names;
    std::vector<std::string> toolbar_paths;

    while (std::getline(in, line)) {
        auto key_pos = line.find("mod_order");
        if (key_pos != std::string::npos) {
            auto bracket = line.find('[');
            auto close_bracket = line.find(']');
            if (bracket != std::string::npos && close_bracket != std::string::npos) {
                auto content = line.substr(bracket + 1, close_bracket - bracket - 1);
                std::istringstream ss(content);
                std::string token;
                while (std::getline(ss, token, ',')) {
                    auto s = token.find_first_not_of(" \t\"");
                    auto e = token.find_last_not_of(" \t\"");
                    if (s != std::string::npos && e != std::string::npos) {
                        order.push_back(token.substr(s, e - s + 1));
                    }
                }
            }
            continue;
        }

        auto fold_pos = line.find("folded_separators");
        if (fold_pos != std::string::npos) {
            auto bracket = line.find('[');
            auto close_bracket = line.find(']');
            if (bracket != std::string::npos && close_bracket != std::string::npos) {
                auto content = line.substr(bracket + 1, close_bracket - bracket - 1);
                std::istringstream ss(content);
                std::string token;
                while (std::getline(ss, token, ',')) {
                    auto s = token.find_first_not_of(" \t\"");
                    auto e = token.find_last_not_of(" \t\"");
                    if (s != std::string::npos && e != std::string::npos) {
                        folded_names.push_back(token.substr(s, e - s + 1));
                    }
                }
            }
            continue;
        }

        auto ts_pos = line.find("toolbar_shortcuts");
        if (ts_pos != std::string::npos) {
            auto bracket = line.find('[');
            auto close_bracket = line.find(']');
            if (bracket != std::string::npos && close_bracket != std::string::npos) {
                auto content = line.substr(bracket + 1, close_bracket - bracket - 1);
                std::istringstream ss(content);
                std::string token;
                while (std::getline(ss, token, ',')) {
                    auto s = token.find_first_not_of(" \t\"");
                    auto e = token.find_last_not_of(" \t\"");
                    if (s != std::string::npos && e != std::string::npos) {
                        toolbar_paths.push_back(token.substr(s, e - s + 1));
                    }
                }
            }
            continue;
        }
    }

    in.close();

    loading_ = true;

    auto mods = mod_model_->mods();
    if (mods.isEmpty()) {
        loading_ = false;
        return;
    }

    // Migration path: if old mod_order is present, use it to reorder + assign priorities
    auto apply_fold = [&folded_names](ModEntry& m) {
        if (!m.is_separator) return;
        m.folded = false;
        for (const auto& fn : folded_names) {
            if (m.name.toStdString() == fn) { m.folded = true; break; }
        }
    };

    if (!order.empty()) {
        QMap<QString, int> id_to_idx;
        for (int i = 0; i < mods.size(); ++i) {
            if (!mods[i].is_overwrite)
                id_to_idx[mods[i].id] = i;
        }
        QVector<ModEntry> reordered;
        // Saved order
        int prio = 1;
        for (const auto& folder_str : order) {
            auto folder_id = QString::fromStdString(folder_str);
            if (id_to_idx.contains(folder_id)) {
                auto entry = mods[id_to_idx[folder_id]];
                entry.priority = prio++;
                apply_fold(entry);
                reordered.append(entry);
                id_to_idx.remove(folder_id);
            }
        }
        // Remaining (new) entries
        for (auto& m : mods) {
            if (id_to_idx.contains(m.id)) {
                m.priority = prio++;
                apply_fold(m);
                reordered.append(m);
            }
        }
        // Overwrite always at bottom
        for (const auto& m : mods) {
            if (m.is_overwrite) { reordered.append(m); break; }
        }
        mod_model_->reset_with_order(reordered);
        mod_model_->renumber_priorities();
        engine::Logger::instance().debug("Migrated from mod_order (" + std::to_string(order.size()) + " entries)");
    } else {
        // New path: sort by priority from meta.ini
        QVector<ModEntry> sorted = mods;
        std::stable_sort(sorted.begin(), sorted.end(), [](const ModEntry& a, const ModEntry& b) {
            if (a.is_overwrite) return false;
            if (b.is_overwrite) return true;
            return a.priority < b.priority;
        });
        bool needs_sort = false;
        for (int i = 0; i < mods.size(); ++i) {
            if (mods[i].id != sorted[i].id) { needs_sort = true; break; }
        }
        if (needs_sort) {
            for (auto& m : sorted) apply_fold(m);
            mod_model_->reset_with_order(sorted);
            mod_model_->renumber_priorities();
        } else {
            // Apply fold states directly to model
            for (int i = 0; i < mod_model_->mods().size(); ++i) {
                ModEntry copy = mod_model_->mods()[i];
                apply_fold(copy);
                mod_model_->set_folded(i, copy.folded);
            }
        }
    }

    // Ensure apply_fold_state() reflects current flags
    mod_model_->apply_fold_state();

    mod_model_->apply_fold_state();

    // Restore toolbar shortcuts
    engine::Logger::instance().begin_group(engine::LogLevel::Debug, "Restored toolbar shortcuts");
    for (const auto& path : toolbar_paths) {
        add_toolbar_shortcut_from_path(QString::fromStdString(path));
    }
    engine::Logger::instance().end_group();

    loading_ = false;

    // Log final model order
    {
        std::ostringstream dbg;
        for (int i = 0; i < mods.size(); ++i) {
            if (i > 0) dbg << ", ";
            dbg << mods[i].id.toStdString() << "(" << mods[i].priority << ")";
            if (mods[i].is_separator) dbg << "[SEP]";
        }
        //engine::Logger::instance().debug("MODEL_ORDER: [" + dbg.str() + "]");
    }

    sync_separator_ids();
}

void MainWindow::save_executables() {
    if (current_instance_root_.empty()) return;

    auto execs = right_panel_->exec_controls()->executable_paths();
    auto toml_path = current_instance_root_ / "instance.toml";

    // Read existing toml to preserve other fields
    std::ifstream in(toml_path);
    std::string existing;
    if (in) {
        existing.assign((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    }
    in.close();

    // Remove old executables lines
    std::istringstream stream(existing);
    std::string line;
    std::string cleaned;
    while (std::getline(stream, line)) {
        auto key_pos = line.find("executables");
        if (key_pos != std::string::npos) continue;
        cleaned += line + "\n";
    }

    // Write saved executables
    cleaned += "executables = [";
    for (int i = 0; i < execs.size(); ++i) {
        if (i > 0) cleaned += ", ";
        cleaned += "\"" + execs[i].toStdString() + "\"";
    }
    cleaned += "]\n";

    std::ofstream out(toml_path);
    if (out) out << cleaned;
}

void MainWindow::load_executables() {
    saved_executables_.clear();
    if (current_instance_root_.empty()) return;

    auto toml_path = current_instance_root_ / "instance.toml";
    std::ifstream in(toml_path);
    if (!in) return;

    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());

    // Find executables = [...] line
    auto start = content.find("executables = [");
    if (start == std::string::npos) return;
    start += std::string("executables = [").size();
    auto end = content.find(']', start);
    if (end == std::string::npos) return;

    auto section = content.substr(start, end - start);
    std::istringstream ss(section);
    std::string token;
    while (std::getline(ss, token, ',')) {
        auto q = token.find('"');
        auto q2 = token.rfind('"');
        if (q != std::string::npos && q2 != std::string::npos && q2 > q) {
            saved_executables_.push_back(token.substr(q + 1, q2 - q - 1));
        }
    }
}

void MainWindow::sync_separator_ids() {
    if (current_instance_root_.empty()) return;
    auto meta_dir = meta_dir_path();
    if (meta_dir.empty()) return;

    const auto& mods = mod_model_->mods();
    QString current_sep_id;
    for (int i = 0; i < mods.size(); ++i) {
        const auto& m = mods[i];
        if (m.is_separator) {
            current_sep_id = m.id;
            mod_model_->set_separator_id(m.id, m.id);
        } else if (m.is_overwrite) {
            mod_model_->set_separator_id(m.id, QString());
        } else {
            QString new_sid = current_sep_id.isEmpty() ? QString() : current_sep_id;
            if (m.separator_id != new_sid) {
                mod_model_->set_separator_id(m.id, new_sid);
                // Persist to meta.ini
                auto folder_name = m.id.toStdString();
                auto meta = engine::ModMeta::load(meta_dir, folder_name);
                meta.set_separator_id(new_sid.toStdString());
                meta.save(meta_dir, folder_name);
            }
        }
    }
}

void MainWindow::group_mods_by_separator() {
    const auto& mods = mod_model_->mods();
    if (mods.isEmpty()) return;
    auto meta_dir = meta_dir_path();
    if (meta_dir.empty()) return;

    // Collect separators first (in their current order)
    QVector<ModEntry> separators;
    QVector<ModEntry> ungrouped;
    QMap<QString, QVector<ModEntry>> grouped;  // separator_id → mods

    ModEntry overwrite_entry;
    bool has_overwrite = false;

    for (const auto& m : mods) {
        if (m.is_separator) {
            separators.append(m);
        } else if (m.is_overwrite) {
            overwrite_entry = m;
            has_overwrite = true;
        } else {
            // Read separator_id from this mod's meta.ini
            auto meta = engine::ModMeta::load(meta_dir, m.id.toStdString());
            auto sid = QString::fromStdString(meta.separator_id());
            if (!sid.isEmpty()) {
                grouped[sid].append(m);
            } else {
                ungrouped.append(m);
            }
        }
    }

    // Rebuild: separators with their grouped mods, then ungrouped, then Overwrite at bottom
    QVector<ModEntry> reordered;

    for (const auto& sep : separators) {
        reordered.append(sep);
        auto it = grouped.find(sep.id);
        if (it != grouped.end()) {
            for (auto& m : it.value()) {
                m.separator_id = sep.id;
                reordered.append(m);
            }
            grouped.erase(it);
        }
    }

    // Any remaining grouped mods whose separator no longer exists → append ungrouped
    for (auto it = grouped.begin(); it != grouped.end(); ++it) {
        for (auto& m : it.value()) {
            m.separator_id.clear();
            ungrouped.append(m);
        }
    }

    for (auto& m : ungrouped)
        reordered.append(m);

    // Overwrite always at bottom
    if (has_overwrite)
        reordered.append(overwrite_entry);

    loading_ = true;
    mod_model_->reset_with_order(reordered);
    loading_ = false;

    engine::Logger::instance().debug("Grouped mods by separator (fallback order)");
}

void MainWindow::populate_executables() {
    if (!knowledge_ || current_game_id_.empty()) return;

    auto default_exec = knowledge_->get(current_game_id_, "default_executable", "");

    // If the configured default doesn't exist on disk, fall back to SkyrimSE.exe
    if (!default_exec.empty() && !current_game_dir_.empty()) {
        auto default_path = current_game_dir_ / default_exec;
        if (!std::filesystem::exists(default_path))
            default_exec = "SkyrimSE.exe";
    }

    // Prefer saved executables list (persists user additions across restarts)
    QStringList exec_list;
    if (!saved_executables_.empty()) {
        for (const auto& s : saved_executables_)
            exec_list.append(QString::fromStdString(s));
    } else {
        // First launch — seed from game plugin's known executables
        auto execs_csv = knowledge_->get(current_game_id_, "executables", "");
        if (!execs_csv.empty()) {
            std::istringstream ss(execs_csv);
            std::string token;
            while (std::getline(ss, token, ',')) {
                auto s = token.find_first_not_of(" \t");
                auto e = token.find_last_not_of(" \t");
                if (s != std::string::npos && e != std::string::npos)
                    exec_list.append(QString::fromStdString(token.substr(s, e - s + 1)));
            }
        }
    }

    auto icon_cache = current_instance_root_.empty()
        ? std::filesystem::path{}
        : current_instance_root_ / "cache" / "thumbnails";
    right_panel_->exec_controls()->set_executables(exec_list, QString::fromStdString(default_exec), current_game_dir_, icon_cache);

    // Persist immediately on first run so future launches use the saved list
    if (saved_executables_.empty())
        save_executables();
}

void MainWindow::launch_game() {
    auto exec_rel = right_panel_->exec_controls()->current_executable();
    if (exec_rel.isEmpty() || exec_rel == "Select executable...") {
        QMessageBox::warning(this, "Launch", "No executable selected.");
        return;
    }
    if (current_game_dir_.empty()) {
        QMessageBox::warning(this, "Launch", "Game directory not set.");
        return;
    }

    auto exec_path = current_game_dir_ / exec_rel.toStdString();
    launch_with_executable(QString::fromStdString(exec_path.string()));
}

static void gmm_debug(const char* fmt, ...) {
    static bool enabled = (std::getenv("GMM_OVERLAY_DEBUG") != nullptr);
    if (!enabled) return;
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[GMM] ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}

void MainWindow::launch_with_executable(const QString& full_path) {
    auto exec_path = std::filesystem::path(full_path.toStdString());
    if (!std::filesystem::exists(exec_path)) {
        QMessageBox::warning(this, "Launch",
            "Executable not found:\n" + full_path);
        return;
    }

    // Read steam_appid from game plugin hooks — 0 if not registered
    uint32_t steam_appid = 0;
    if (knowledge_) {
        auto id_str = knowledge_->get(current_game_id_, "steam_appid", "");
        if (!id_str.empty()) {
            try { steam_appid = std::stoul(id_str); } catch (...) {}
        }
    }

    engine::LaunchParams lparams;
    lparams.executable = exec_path;
    lparams.game_dir = current_game_dir_;
    lparams.overwrite_dir = current_instance_root_ / "overwrite";
    lparams.steam_appid = steam_appid;
    lparams.is_windows_exe = (exec_path.extension().string() == ".exe" ||
                              exec_path.extension().string() == ".EXE");
    if (!staging_dir_.empty()) {
        lparams.extra_lowerdirs.push_back(staging_dir_);
    }

    auto lresult = engine::launch_game(lparams);

    if (lresult.pid <= 0) {
        QMessageBox::warning(this, "Launch", "Failed to launch game.");
        return;
    }

    overlay_launched_ = lresult.overlay_launched;
    running_process_pid_ = lresult.pid;
    launch_time_ = std::filesystem::file_time_type::clock::now();

    if (!process_watch_timer_) {
        process_watch_timer_ = new QTimer(this);
        process_watch_timer_->setInterval(2000);
        connect(process_watch_timer_, &QTimer::timeout, this, [this]() {
            check_running_process();
        });
    }
    process_watch_timer_->start();
    show_game_lock_overlay(QString::fromStdString(exec_path.filename().string()), lresult.pid);
}

void MainWindow::check_running_process() {
    if (running_process_pid_ <= 0) {
        engine::Logger::instance().warn("Watchdog: pid <= 0, stopping timer");
        if (process_watch_timer_) process_watch_timer_->stop();
        return;
    }

#ifdef _WIN32
    HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE,
                              static_cast<DWORD>(running_process_pid_));
    bool alive = false;
    if (proc) {
        DWORD exit_code;
        if (GetExitCodeProcess(proc, &exit_code) && exit_code == STILL_ACTIVE)
            alive = true;
        CloseHandle(proc);
    }
    if (!alive) {
        running_process_pid_ = -1;
        if (process_watch_timer_) process_watch_timer_->stop();
        flush_pending_changes();
        hide_game_lock_overlay();
        if (!staging_dir_.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(staging_dir_, ec);
            staging_dir_.clear();
        }
        // Delay capture so any child/spawned processes finish writing
        auto t = launch_time_;
        QTimer::singleShot(3000, this, [this, t]() { do_capture_overwrite(t); });
    }
#else
    // Linux: track the entire process group (PGID), not a single PID.
    int64_t pgid = running_process_pid_;

    if (engine::is_process_group_alive(pgid)) {
        if (process_tree_checkbox_ && process_tree_checkbox_->isChecked())
            refresh_process_tree();
        return;
    }

    // PGID scan found nothing — try PPID descendant walk.
    // This finds processes that created new sessions via setsid().
    auto descendants = engine::get_process_descendants(pgid);
    bool found_alive = false;
    for (int64_t dpid : descendants) {
        if (dpid == pgid) continue;
        if (kill(static_cast<pid_t>(dpid), 0) == 0 || errno == EPERM) {
            found_alive = true;
            break;
        }
    }

    if (process_tree_checkbox_ && process_tree_checkbox_->isChecked())
        refresh_process_tree();

    if (found_alive) return;

    engine::Logger::instance().info("Watchdog: process group " +
        std::to_string(pgid) + " fully exited, scheduling capture in 3s");
    flush_pending_changes();
    hide_game_lock_overlay();
    running_process_pid_ = -1;
    if (process_watch_timer_) process_watch_timer_->stop();
    // Clean up overlay staging dir (mod symlinks) — no longer needed
    if (!staging_dir_.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(staging_dir_, ec);
        staging_dir_.clear();
    }
    auto t = launch_time_;
    QTimer::singleShot(3000, this, [this, t]() { do_capture_overwrite(t); });
#endif
}

void MainWindow::copy_process_tree() {
    if (!process_tree_) return;

    QStringList lines;
    std::function<void(QTreeWidgetItem*, int)> walk = [&](QTreeWidgetItem* item, int depth) {
        QString indent(depth * 2, ' ');
        lines << indent + item->text(0) + "  (PID " + item->text(1) + "  " + item->text(2) + ")";
        for (int i = 0; i < item->childCount(); ++i)
            walk(item->child(i), depth + 1);
    };

    for (int i = 0; i < process_tree_->topLevelItemCount(); ++i)
        walk(process_tree_->topLevelItem(i), 0);

    QApplication::clipboard()->setText(lines.join("\n"));
}

void MainWindow::refresh_process_tree() {
    if (!process_tree_) return;
    process_tree_->clear();

    pid_t root_pid = static_cast<pid_t>(running_process_pid_);
    if (root_pid <= 0) return;

    // Read root_pid's session ID — all descendants inherit this via setsid()
    pid_t root_session = -1;
    {
        std::string spath = "/proc/" + std::to_string(root_pid) + "/stat";
        FILE* f = fopen(spath.c_str(), "r");
        if (f) {
            char buf[4096] = {};
            size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            fclose(f);
            buf[n] = '\0';
            char* open_paren = strchr(buf, '(');
            char* close_paren = strrchr(buf, ')');
            if (open_paren && close_paren) {
                char* p = close_paren + 1;
                while (*p == ' ') ++p;
                while (*p && *p != ' ') ++p;
                while (*p == ' ') ++p;
                while (*p && *p != ' ') ++p;
                while (*p == ' ') ++p;
                while (*p && *p != ' ') ++p;
                while (*p == ' ') ++p;
                root_session = static_cast<pid_t>(atol(p));
            }
        }
    }

    struct ProcInfo { pid_t pid; pid_t ppid; std::string name; char state; };
    std::vector<ProcInfo> procs;

    // Collect all processes from /proc
    DIR* dir = opendir("/proc");
    if (!dir) return;
    struct dirent* entry;
    while ((entry = readdir(dir))) {
        if (entry->d_type != DT_DIR) continue;
        pid_t pid = atol(entry->d_name);
        if (pid <= 0) continue;

        std::string spath = "/proc/" + std::to_string(pid) + "/stat";
        FILE* f = fopen(spath.c_str(), "r");
        if (!f) continue;
        char buf[4096] = {};
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = '\0';

        char* open_paren = strchr(buf, '(');
        char* close_paren = strrchr(buf, ')');
        if (!open_paren || !close_paren) continue;

        std::string comm(open_paren + 1, close_paren - open_paren - 1);
        char* p = close_paren + 1;
        while (*p == ' ') ++p;
        char state = *p;

        while (*p && *p != ' ') ++p;
        while (*p == ' ') ++p;
        pid_t ppid = static_cast<pid_t>(atol(p));
        while (*p && *p != ' ') ++p;
        while (*p == ' ') ++p;
        pid_t session = static_cast<pid_t>(atol(p));

        // Filter: if we got root's SID, include only same-session processes
        if (root_session > 0 && session != root_session)
            continue;

        procs.push_back({pid, ppid, comm, state});
    }
    closedir(dir);

    // If SID couldn't be read (root exited), fall back to PPID descendant walk
    if (root_session <= 0) {
        std::unordered_set<pid_t> descendants;
        descendants.insert(root_pid);
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& pr : procs) {
                if (descendants.count(pr.pid)) continue;
                if (descendants.count(pr.ppid)) {
                    descendants.insert(pr.pid);
                    changed = true;
                }
            }
        }
        procs.erase(std::remove_if(procs.begin(), procs.end(),
            [&](const ProcInfo& pr) { return !descendants.count(pr.pid); }),
            procs.end());
    }

    if (procs.empty()) return;

    std::unordered_map<pid_t, QTreeWidgetItem*> items;

    // First pass: create all items
    for (const auto& pr : procs) {
        auto* item = new QTreeWidgetItem;
        item->setText(0, QString::fromStdString(pr.name));
        item->setText(1, QString::number(pr.pid));
        item->setText(2, QString(QChar(pr.state)));
        items[pr.pid] = item;
    }

    // Second pass: build parent-child relationships
    for (const auto& pr : procs) {
        auto* item = items[pr.pid];
        auto parent_it = items.find(pr.ppid);
        if (parent_it != items.end()) {
            parent_it->second->addChild(item);
        } else {
            process_tree_->addTopLevelItem(item);
        }
    }

    process_tree_->expandAll();
}

void MainWindow::capture_overwrite_on_exit() {
    do_capture_overwrite(launch_time_);
}

void MainWindow::do_capture_overwrite(std::filesystem::file_time_type capture_time) {
    if (current_instance_root_.empty() || current_game_dir_.empty()) return;

    // When launched via overlay, all writes already went directly to Overwrite.
    if (overlay_launched_) {
        overlay_launched_ = false;
        engine::Logger::instance().debug("Overlay launched: reloading mods (writes already in Overwrite)");
        load_mods_from_game();
        return;
    }

    auto overwrite_dir = current_instance_root_ / "overwrite";
    engine::capture_overwrite(current_game_dir_, overwrite_dir, capture_time);

    // Reload mod list so Overwrite contents become visible
    std::error_code ec;
    if (std::filesystem::exists(overwrite_dir, ec)) {
        load_mods_from_game();
    }
}

void MainWindow::add_shortcut_to_toolbar() {
    auto exec_rel = right_panel_->exec_controls()->current_executable();
    if (exec_rel.isEmpty() || exec_rel == "Select executable...") {
        QMessageBox::warning(this, "Shortcut", "No executable selected.");
        return;
    }
    if (current_game_dir_.empty()) {
        QMessageBox::warning(this, "Shortcut", "Game directory not set.");
        return;
    }

    auto exec_path = current_game_dir_ / exec_rel.toStdString();
    if (!std::filesystem::exists(exec_path)) {
        QMessageBox::warning(this, "Shortcut",
            "Executable not found:\n" + QString::fromStdString(exec_path.string()));
        return;
    }

    // Deduplicate: silently ignore if already added
    auto exec_path_qstr = QString::fromStdString(exec_path.string());
    add_toolbar_shortcut_from_path(exec_path_qstr);
}

void MainWindow::add_toolbar_shortcut_from_path(const QString& full_path) {
    if (toolbar_shortcut_paths_.contains(full_path)) return;
    if (!QFileInfo::exists(full_path)) return;

    auto icon = extractExeIconShortcut(full_path);

    // Derive tooltip from game name (replace underscores with spaces) + exe filename
    auto info = QFileInfo(full_path);
    auto game_name = QString::fromStdString(current_game_name_.empty() ? current_game_id_ : current_game_name_);
    game_name.replace('_', ' ');
    auto tooltip = game_name + " \u2014 " + info.fileName();

    auto* btn = toolbar_->add_exec_button(tooltip, icon);
    btn->setProperty("exec_path", full_path);
    connect(btn, &QToolButton::clicked, this, [this, full_path]() {
        launch_with_executable(full_path);
    });
    toolbar_shortcut_paths_.append(full_path);
    save_order();
}

void MainWindow::add_shortcut_to_desktop() {
    auto exec_rel = right_panel_->exec_controls()->current_executable();
    if (exec_rel.isEmpty() || exec_rel == "Select executable...") {
        QMessageBox::warning(this, "Shortcut", "No executable selected.");
        return;
    }
    if (current_game_dir_.empty()) {
        QMessageBox::warning(this, "Shortcut", "Game directory not set.");
        return;
    }

    auto exec_path = current_game_dir_ / exec_rel.toStdString();
    if (!std::filesystem::exists(exec_path)) {
        QMessageBox::warning(this, "Shortcut",
            "Executable not found:\n" + QString::fromStdString(exec_path.string()));
        return;
    }

    auto game_name = QString::fromStdString(current_game_name_.empty() ? current_game_id_ : current_game_name_);

    // Find the user's desktop directory (XDG or fallback to ~/Desktop)
    auto desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    if (desktop.isEmpty()) {
        auto* home = std::getenv("HOME");
        if (home) desktop = QString::fromUtf8(home) + "/Desktop";
    }
    if (desktop.isEmpty()) {
        QMessageBox::warning(this, "Shortcut", "Could not determine desktop directory.");
        return;
    }

    // Build the .desktop file content (XDG standard)
    auto exec_qstr = QString::fromStdString(exec_path.string());
    // Escape % sign for .desktop files
    exec_qstr.replace("%", "%%");

    auto desktop_file = desktop + "/" + game_name.replace(" ", "_") + ".desktop";
    QFile f(desktop_file);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, "Shortcut",
            "Failed to create desktop file:\n" + desktop_file);
        return;
    }

    QTextStream out(&f);
    out << "[Desktop Entry]\n";
    out << "Type=Application\n";
    out << "Name=" << game_name << "\n";
    out << "Exec=" << exec_qstr << "\n";
    out << "Path=" << QString::fromStdString(current_game_dir_.string()) << "\n";
    out << "Icon=" << QString::fromStdString(exec_path.string()) << "\n";
    out << "Terminal=false\n";
    out << "Categories=Game;\n";
    out << "Comment=Launch " << game_name << " via GameModManager\n";
    f.close();

    // Make it executable
    QFile::setPermissions(desktop_file,
        QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner |
        QFile::ReadGroup | QFile::ExeGroup |
        QFile::ReadOther | QFile::ExeOther);

    engine::Logger::instance().debug("Created desktop shortcut: " + desktop_file.toStdString());
    QMessageBox::information(this, "Shortcut",
        "Desktop shortcut created:\n" + desktop_file);
}

void MainWindow::pick_executable_file() {
    auto start_dir = current_game_dir_.empty()
        ? QDir::homePath()
        : QString::fromStdString(current_game_dir_.string());

#ifdef Q_OS_WIN
    QString filter = "Executables (*.exe);;All Files (*)";
#else
    QString filter = "Executables (*.exe *.AppImage *.bin *.elf *.sh);;All Files (*)";
#endif

    auto path = QFileDialog::getOpenFileName(this,
        "Select Executable", start_dir, filter);

    if (path.isEmpty()) return;

#ifndef Q_OS_WIN
    if (!validate_linux_executable(path)) {
        QMessageBox::warning(this, "Select Executable",
            "The selected file is not a recognized executable type.\n"
            "Please select a binary (.exe, .elf, .sh, .AppImage, .bin)\n"
            "or an extensionless executable file.");
        return;
    }
#endif

    // Compute relative path from game dir
    QString rel;
    if (!current_game_dir_.empty()) {
        auto game_qdir = QDir(start_dir);
        rel = game_qdir.relativeFilePath(path);
    } else {
        rel = path;
    }

    auto display = QFileInfo(path).fileName();

    // Try to extract icon from the selected file
    QIcon icon;
    if (std::filesystem::exists(path.toStdString())) {
        QFileIconProvider provider;
        icon = provider.icon(QFileInfo(path));
    }

    right_panel_->exec_controls()->add_executable(display, rel, icon);
    save_executables();
}

#ifndef Q_OS_WIN
bool MainWindow::validate_linux_executable(const QString& path) {
    QFileInfo fi(path);
    if (!fi.exists()) return false;

    // Check extension-based patterns first (fast path)
    auto ext = fi.suffix().toLower();
    if (ext == "exe" || ext == "elf" || ext == "sh"
        || ext == "appimage" || ext == "bin")
        return true;

    // For extensionless files, use `file --brief --mime-type`
    QProcess proc;
    proc.start("file", QStringList{"--brief", "--mime-type", path});
    if (!proc.waitForFinished(3000)) return false;

    auto mime = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
    return mime == "application/x-executable"
        || mime == "application/x-pie-executable"
        || mime == "application/x-sharedlib"
        || mime == "text/x-shellscript"
        || mime == "application/x-mach-binary"
        || mime == "application/x-msdownload"
        || mime == "application/x-msdos-program";
}
#endif

std::filesystem::path MainWindow::app_state_path() const {
    if (current_instance_root_.empty()) return {};
    return current_instance_root_ / "config" / "app_state.dat";
}

// --- Game-lock overlay ---

void MainWindow::create_game_lock_overlay() {
    game_lock_overlay_ = new QWidget(this);
    game_lock_overlay_->setObjectName("gameLockOverlay");

    auto* layout = new QVBoxLayout(game_lock_overlay_);
    layout->setAlignment(Qt::AlignCenter);
    layout->setContentsMargins(40, 40, 40, 40);

    layout->addStretch(2);

    game_lock_label_ = new QLabel("The game is running", game_lock_overlay_);
    game_lock_label_->setAlignment(Qt::AlignCenter);
    layout->addWidget(game_lock_label_);

    pending_queue_label_ = new QLabel(game_lock_overlay_);
    pending_queue_label_->setAlignment(Qt::AlignCenter);
    pending_queue_label_->setStyleSheet("color: #f0b000; font-weight: bold;");
    pending_queue_label_->hide();
    layout->addWidget(pending_queue_label_);

    auto* tree_row = new QHBoxLayout;
    tree_row->setAlignment(Qt::AlignCenter);

    process_tree_checkbox_ = new QCheckBox(game_lock_overlay_);
    process_tree_checkbox_->setChecked(show_process_tree_);
    QObject::connect(process_tree_checkbox_, &QCheckBox::toggled, this, [this](bool checked) {
        show_process_tree_ = checked;
        if (process_tree_) {
            process_tree_->setVisible(checked);
            if (checked) refresh_process_tree();
        }
    });
    tree_row->addWidget(process_tree_checkbox_);

    auto* tree_label = new QLabel("Show process tree", game_lock_overlay_);
    tree_label->setObjectName("processTreeLabel");
    tree_row->addWidget(tree_label);

    layout->addLayout(tree_row);

    process_tree_ = new QTreeWidget(game_lock_overlay_);
    process_tree_->setHeaderLabels({"Name", "PID", "S"});
    process_tree_->setColumnWidth(0, 200);
    process_tree_->setColumnWidth(1, 80);
    process_tree_->setColumnWidth(2, 30);
    process_tree_->setAlternatingRowColors(false);
    process_tree_->setRootIsDecorated(true);
    process_tree_->setAnimated(true);
    process_tree_->header()->setStretchLastSection(false);
    process_tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    process_tree_->setVisible(show_process_tree_);
    process_tree_->setMaximumHeight(250);
    layout->addWidget(process_tree_);

    // Copy button row (bottom-right of tree)
    auto* copy_tree_row = new QHBoxLayout;
    copy_tree_row->setContentsMargins(0, 0, 0, 0);
    auto* copy_tree_btn = new QPushButton("Copy", game_lock_overlay_);
    copy_tree_btn->setFixedSize(52, 22);
    copy_tree_btn->setToolTip("Copy process tree structure to clipboard");
    copy_tree_btn->setVisible(show_process_tree_);
    QObject::connect(copy_tree_btn, &QPushButton::clicked, this, &MainWindow::copy_process_tree);
    // Show/hide in sync with the tree toggle
    QObject::connect(process_tree_checkbox_, &QCheckBox::toggled, copy_tree_btn, &QPushButton::setVisible);
    copy_tree_row->addStretch();
    copy_tree_row->addWidget(copy_tree_btn);
    layout->addLayout(copy_tree_row);

    layout->addStretch(1);

    auto* btn_layout = new QHBoxLayout;
    btn_layout->setAlignment(Qt::AlignCenter);
    btn_layout->setSpacing(16);

    unlock_button_ = new QPushButton("Unlock", game_lock_overlay_);
    unlock_button_->setObjectName("unlockBtn");
    QObject::connect(unlock_button_, &QPushButton::clicked, this, [this]() {
        hide_game_lock_overlay();
    });
    btn_layout->addWidget(unlock_button_);

    kill_button_ = new QPushButton("Kill", game_lock_overlay_);
    kill_button_->setObjectName("killBtn");
    QObject::connect(kill_button_, &QPushButton::clicked, this, [this]() {
        pid_t pgid = static_cast<pid_t>(running_process_pid_);
        if (pgid <= 0) {
            flush_pending_changes();
            hide_game_lock_overlay();
            return;
        }
        // Kill the entire process group — game, launchers, Proton wrapper, all children.
        // kill(-pgid, sig) sends to every process in the group.
        // Try graceful shutdown first, then force kill.
        int ret = kill(-pgid, SIGTERM);
        if (ret != 0) {
            int err = errno;
            if (err == ESRCH) {
                engine::Logger::instance().info("Kill: process group " + std::to_string(pgid) + " already empty");
                running_process_pid_ = -1;
                flush_pending_changes();
                hide_game_lock_overlay();
                return;
            }
            engine::Logger::instance().error("Kill: kill(-" + std::to_string(pgid) + ", SIGTERM) failed: " +
                std::strerror(err) + " (" + std::to_string(err) + ")");
            ret = kill(-pgid, SIGKILL);
            if (ret != 0) {
                err = errno;
                engine::Logger::instance().error("Kill: kill(-" + std::to_string(pgid) + ", SIGKILL) failed: " +
                    std::strerror(err) + " (" + std::to_string(err) + ")");
                QMessageBox::warning(this, "Kill Failed",
                    QString("Failed to terminate process group %1: %2")
                        .arg(static_cast<long long>(pgid))
                        .arg(std::strerror(err)));
                return;
            }
        }
        engine::Logger::instance().info("Kill: terminated process group " + std::to_string(pgid));
        if (process_watch_timer_) process_watch_timer_->stop();
        running_process_pid_ = -1;
        flush_pending_changes();
        hide_game_lock_overlay();
    });
    btn_layout->addWidget(kill_button_);

    layout->addLayout(btn_layout);

    game_lock_overlay_->hide();
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    if (game_lock_overlay_)
        game_lock_overlay_->setGeometry(rect());
}

void MainWindow::show_game_lock_overlay(const QString& binary_name, int64_t pid) {
    locked_pid_ = pid;
    pending_changes_.clear();
    if (pending_queue_label_) pending_queue_label_->hide();
    game_lock_label_->setText(QString("The game is running: %1 (%2)")
        .arg(binary_name)
        .arg(pid));
    game_lock_overlay_->setGeometry(rect());
    game_lock_overlay_->raise();
    game_lock_overlay_->show();
    if (process_tree_checkbox_ && process_tree_checkbox_->isChecked())
        refresh_process_tree();
}

void MainWindow::hide_game_lock_overlay() {
    locked_pid_ = -1;
    game_lock_overlay_->hide();
}

void MainWindow::flush_pending_changes() {
    if (pending_changes_.empty()) return;
    if (!knowledge_ || current_game_id_.empty() || current_game_dir_.empty()) return;
    if (!mod_model_) return;

    engine::Logger::instance().info("Flushing " + std::to_string(pending_changes_.size()) +
        " queued mod changes");

    auto mods_subpath = knowledge_->get(current_game_id_, "mods_subpath", "");
    if (mods_subpath.empty()) {
        engine::Logger::instance().warn("Cannot flush changes: mods_subpath is empty");
        pending_changes_.clear();
        return;
    }

    // Apply toggles (latest state per mod wins — already deduplicated by sync_mod_enable_state)
    for (const auto& pt : pending_changes_) {
        auto mod_folder = mods_dir_path() / pt.mod_id.toStdString();
        if (pt.enabled) {
            (void)engine::ModScanner::enable_mod(*knowledge_, current_game_id_, mod_folder);
        } else {
            (void)engine::ModScanner::disable_mod(*knowledge_, current_game_id_, mod_folder);
        }
    }

    // Save final mod order (priorities may have changed via drag-drop while game ran)
    auto saved_pid = running_process_pid_;
    running_process_pid_ = -1;  // bypass game-running guard in sync_priorities
    sync_priorities();
    running_process_pid_ = saved_pid;

    pending_changes_.clear();
    if (pending_queue_label_) pending_queue_label_->hide();
    engine::Logger::instance().info("Queued mod changes flushed");
}

void MainWindow::update_queue_label() {
    if (!pending_queue_label_) return;
    if (pending_changes_.empty()) {
        pending_queue_label_->hide();
        return;
    }
    pending_queue_label_->setText(QString("Changes queued: %1 (apply on game exit)")
        .arg(pending_changes_.size()));
    pending_queue_label_->show();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    save_download_manifest();
    save_app_state();

    // Disconnect pipeline signals to prevent callbacks on a partially-destroyed MainWindow
    if (pipeline_thread_) {
        disconnect(pipeline_thread_->worker(), nullptr, this, nullptr);
        pipeline_thread_->stop();
    }

    // Save mod order before closing
    if (!loading_) {
        save_order();
    }

    QMainWindow::closeEvent(event);
}

bool MainWindow::eventFilter(QObject* obj, QEvent* event) {
    if (event->type() == QEvent::KeyPress && !current_instance_root_.empty()) {
        auto* ke = static_cast<QKeyEvent*>(event);
        int key = ke->key();
        (void)obj;

        if (konami_state_ < 10) {
            if (key == konami_sequence_[konami_state_]) {
                ++konami_state_;
            } else if (key == konami_sequence_[0]) {
                konami_state_ = 1;
            } else {
                konami_state_ = 0;
            }
        }

        if (konami_state_ == 10 && (key == Qt::Key_Enter || key == Qt::Key_Return)) {
            auto flag = current_instance_root_ / "debugging.enabled";
            std::error_code ec;
            if (!std::filesystem::exists(flag, ec)) {
                std::ofstream ofs(flag.string());
                ofs << "enabled\n";
                engine::Logger::instance().info("Debug mode enabled (Konami code entered)");
                status_bar_->set_status("Debug mode enabled");
            } else {
                std::filesystem::remove(flag, ec);
                engine::Logger::instance().info("Debug mode disabled");
                status_bar_->set_status("Debug mode disabled");
            }
            konami_state_ = 0;

            bool enabled = std::filesystem::exists(flag);
            if (enabled) {
                if (!debug_window_) {
                    debug_window_ = new DebugWindow(current_instance_root_,
                        current_game_id_, current_game_name_,
                        plugin_loader_,
                        [this]() { if (style_manager_) style_manager_->reload_current(); }, this);
                }
                debug_window_->show();
                debug_window_->raise();
            } else if (debug_window_) {
                debug_window_->hide();
            }
            return true;
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

void MainWindow::save_app_state() {
    auto path = app_state_path();
    if (path.empty()) return;

    std::ofstream out(path, std::ios::binary);
    if (!out) return;

    // Write window geometry + state
    auto geo = saveGeometry().toBase64();
    auto win_state = saveState().toBase64();
    auto main_split = main_splitter_ ? main_splitter_->saveState().toBase64() : QByteArray();
    auto console_split = console_splitter_ ? console_splitter_->saveState().toBase64() : QByteArray();
    auto header_state = mod_view_ && mod_view_->header()
        ? mod_view_->header()->saveState().toBase64() : QByteArray();

    auto write_ba = [&](const QByteArray& ba) {
        uint32_t len = static_cast<uint32_t>(ba.size());
        out.write(reinterpret_cast<const char*>(&len), sizeof(len));
        if (len > 0) out.write(ba.constData(), len);
    };

    write_ba(geo);
    write_ba(win_state);
    write_ba(main_split);
    write_ba(console_split);
    write_ba(header_state);

    // Save right panel table header states (column widths, order, visibility)
    QJsonObject header_states;
    if (right_panel_) {
        auto* tw = right_panel_->tab_widget();
        for (int i = 0; i < tw->count(); ++i) {
            auto* tab = tw->widget(i);
            auto* table = tab->findChild<QTableWidget*>();
            if (table && table->horizontalHeader()) {
                auto state = table->horizontalHeader()->saveState().toBase64();
                if (!state.isEmpty()) {
                    header_states[tw->tabText(i)] = QString::fromUtf8(state);
                }
            }
        }
    }
    header_states["_process_tree_visible"] = show_process_tree_;
    QByteArray extra = QJsonDocument(header_states).toJson(QJsonDocument::Compact);
    write_ba(extra);

    //engine::Logger::instance().debug("App state saved");
}

void MainWindow::restore_app_state() {
    auto path = app_state_path();
    if (!std::filesystem::exists(path)) return;

    std::ifstream in(path, std::ios::binary);
    if (!in) return;

    auto read_ba = [&]() -> QByteArray {
        uint32_t len = 0;
        if (!in.read(reinterpret_cast<char*>(&len), sizeof(len)) || len == 0) return {};
        std::vector<char> buf(len);
        if (!in.read(buf.data(), len)) return {};
        return QByteArray::fromBase64(QByteArray(buf.data(), len));
    };

    auto geo = read_ba();
    auto win_state = read_ba();
    auto main_split = read_ba();
    auto console_split = read_ba();
    auto header_state = read_ba();

    if (!geo.isEmpty()) pending_geometry_ = geo;
    if (!win_state.isEmpty()) restoreState(win_state);
    if (main_splitter_ && !main_split.isEmpty()) main_splitter_->restoreState(main_split);
    if (console_splitter_ && !console_split.isEmpty()) console_splitter_->restoreState(console_split);
    if (mod_view_ && mod_view_->header() && !header_state.isEmpty()) {
        mod_view_->header()->restoreState(header_state);
    }

    // Restore right panel table header states (column widths, order, visibility)
    uint32_t extra_len = 0;
    if (in.read(reinterpret_cast<char*>(&extra_len), sizeof(extra_len)) && extra_len > 0) {
        std::vector<char> buf(extra_len);
        if (in.read(buf.data(), extra_len)) {
            auto doc = QJsonDocument::fromJson(QByteArray(buf.data(), extra_len));
            if (doc.isObject() && right_panel_) {
            auto obj = doc.object();
            // Restore process tree visibility (prefixed with _ to avoid tab-name collision)
            if (obj.contains("_process_tree_visible"))
                show_process_tree_ = obj["_process_tree_visible"].toBool();
            auto* tw = right_panel_->tab_widget();
                for (int i = 0; i < tw->count(); ++i) {
                    auto key = tw->tabText(i);
                    if (!obj.contains(key)) continue;
                    auto* tab = tw->widget(i);
                    auto* table = tab->findChild<QTableWidget*>();
                    if (table && table->horizontalHeader()) {
                        auto state = QByteArray::fromBase64(
                            obj[key].toString().toUtf8());
                        if (!state.isEmpty())
                            table->horizontalHeader()->restoreState(state);
                    }
                }
            }
        }
    }

    //engine::Logger::instance().debug("App state restored");
}

void MainWindow::apply_initial_geometry() {
    if (!pending_geometry_.isEmpty()) {
        restoreGeometry(pending_geometry_);
        pending_geometry_.clear();
    }
}

std::filesystem::path MainWindow::download_manifest_path() const {
    if (current_instance_root_.empty()) return {};
    return current_instance_root_ / "downloads" / ".download_manifest.json";
}

void MainWindow::save_download_manifest() {
    auto* dt = right_panel_->downloads_tab();
    if (!dt) return;
    auto path = download_manifest_path();
    if (path.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return;
    std::ofstream out(path);
    if (!out) return;
    out << dt->serialize();
}

void MainWindow::load_download_manifest() {
    auto path = download_manifest_path();
    if (path.empty() || !std::filesystem::exists(path)) return;
    std::ifstream in(path);
    if (!in) return;
    std::string json((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
    if (json.empty()) return;
    auto* dt = right_panel_->downloads_tab();
    if (!dt) return;
    auto downloads_dir = current_instance_root_ / "downloads";
    dt->deserialize(json, downloads_dir);
}

void MainWindow::handle_nxm_download(const engine::NxmLink& link) {
    if (!link.valid()) {
        engine::Logger::instance().warn("Invalid NXM link received");
        return;
    }

    engine::Logger::instance().debug(
        "NXM download: domain=" + link.nexus_domain +
        " mod=" + std::to_string(link.mod_id) +
        " file=" + std::to_string(link.file_id));

    // Find which game_id owns this nexus_domain via managed games
    std::string matched_game_id;
    if (managed_games_) {
        matched_game_id = managed_games_->game_id_for_domain(link.nexus_domain);
    }

    // Fallback: try matching via loaded plugins
    if (matched_game_id.empty() && plugin_loader_) {
        for (const auto& p : plugin_loader_->plugins()) {
            if (p.nexus_domain == link.nexus_domain) {
                matched_game_id = p.game_id;
                break;
            }
        }
    }

    if (matched_game_id.empty()) {
        QMessageBox::warning(this, "NXM Download",
            "Unknown Nexus Mods domain: " + QString::fromStdString(link.nexus_domain) +
            "\nNo game plugin supports this domain.");
        return;
    }

    // Is this game managed by us?
    bool is_managed = managed_games_ && managed_games_->is_managed(matched_game_id);

    if (!is_managed) {
        QMessageBox::information(this, "NXM Download",
            "This mod is for " +
            QString::fromStdString(plugin_loader_->display_name_for(matched_game_id)) +
            ", but GameModManager is not managing this game.\n\n"
            "Open the game's instance first to register it, then try the link again.");
        return;
    }

    // Game is managed — but is the active instance the right one?
    if (matched_game_id != current_game_id_) {
        QMessageBox::information(this, "NXM Download",
            "This mod is for " +
            QString::fromStdString(plugin_loader_->display_name_for(matched_game_id)) +
            ", but the active instance is " +
            QString::fromStdString(current_game_name_) +
            ".\nSwitch to the correct instance first.");
        return;
    }

    // Route to the active instance — start download via pipeline worker
    engine::Logger::instance().debug(
        "Starting download: " + current_game_name_ +
        " (mod_id=" + std::to_string(link.mod_id) +
        ", file_id=" + std::to_string(link.file_id) + ")");

    // Show in DownloadsTab immediately
    auto* dt = right_panel_->downloads_tab();
    if (dt) {
        dt->add_download(
            std::to_string(link.mod_id),
            "Mod #" + std::to_string(link.mod_id),
            "Nexus Mods");
    }

    // Build paths for the pipeline context
    auto mods_dir = mods_dir_path();

    // Invoke the pipeline worker asynchronously
    QMetaObject::invokeMethod(pipeline_thread_->worker(), [this, link, mods_dir]() {
        pipeline_thread_->worker()->install_from_nxm(
            link,
            current_game_id_,
            mods_dir.string(),
            current_instance_root_.empty() ? "" : (current_instance_root_ / "meta").string());
    }, Qt::QueuedConnection);

    engine::Logger::instance().debug("Download queued for mod " + std::to_string(link.mod_id));
}

void MainWindow::flush_pending_nxm() {
    if (pending_nxm_url_.empty()) return;

    auto link = engine::NxmRouter::parse(pending_nxm_url_);
    pending_nxm_url_.clear();

    if (link.valid()) {
        handle_nxm_download(link);
    }
}

void MainWindow::prompt_nxm_registration() {
    if (!managed_games_ || !plugin_loader_ || current_game_id_.empty()) return;

    // Already registered? Skip.
    if (managed_games_->is_managed(current_game_id_)) return;

    // Find the nexus_domain for this game from the loaded plugin
    std::string nexus_domain;
    for (const auto& p : plugin_loader_->plugins()) {
        if (p.game_id == current_game_id_) {
            nexus_domain = p.nexus_domain;
            break;
        }
    }

    // No nexus_domain means this game doesn't support Nexus Mods at all
    if (nexus_domain.empty()) return;

    QMessageBox msg(this);
    msg.setWindowTitle("NXM Protocol Handler");
    msg.setText("Do you want to register " + QString::fromStdString(current_game_name_) +
        " to handle download links from <b>nexusmods.com</b> (nxm://)?");
    msg.setTextFormat(Qt::RichText);
    msg.setIcon(QMessageBox::Question);
    msg.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    msg.setDefaultButton(QMessageBox::Yes);
    auto reply = msg.exec();

    if (reply == QMessageBox::Yes) {
        managed_games_->add_source(current_game_id_,
            engine::GameSource{"nexus", "nexusmods.com", nexus_domain});

        // Ensure we're the nxm:// and gmm:// handler on Linux
#ifdef GMM_PLATFORM_LINUX
        auto app_path = std::filesystem::path(
            QCoreApplication::applicationFilePath().toStdString());
        if (!engine::LinuxPlatform::is_nxm_handler_registered()) {
            (void)engine::LinuxPlatform::register_nxm_handler(app_path);
        }
        if (!engine::LinuxPlatform::is_gmm_handler_registered()) {
            (void)engine::LinuxPlatform::register_gmm_handler(app_path);
        }
#endif
        engine::Logger::instance().debug(
            "Registered " + current_game_id_ + " for nxm:// handling (nexusmods.com)");
    }
}

void MainWindow::show_settings_dialog() {
    auto& auth = engine::NexusAuth::instance();
    bool has_key = auth.has_api_key();

    QDialog dlg(this);
    dlg.setWindowTitle("Settings");
    dlg.setMinimumWidth(480);

    auto* layout = new QVBoxLayout(&dlg);

    // -- API Key section ----------------------------------------
    auto* api_group = new QGroupBox("Nexus Mods API Key");
    auto* api_layout = new QVBoxLayout(api_group);

    auto* key_edit = new QLineEdit;
    key_edit->setEchoMode(QLineEdit::Password);
    key_edit->setPlaceholderText("Enter your Nexus Mods API key...");
    if (has_key)
        key_edit->setText(QString::fromStdString(auth.get_api_key()));

    auto* key_row = new QHBoxLayout;
    key_row->addWidget(key_edit, 1);

    auto* save_btn = new QPushButton(has_key ? "Update" : "Save");
    auto* clear_btn = new QPushButton("Clear");
    clear_btn->setEnabled(has_key);
    key_row->addWidget(save_btn);
    key_row->addWidget(clear_btn);

    api_layout->addLayout(key_row);
    api_layout->addWidget(new QLabel(
        "Get your key at "
        "<a href='https://www.nexusmods.com/users/myaccount?tab=api'>"
        "nexusmods.com/users/myaccount?tab=api</a>"));
    layout->addWidget(api_group);

    // -- Rate-limit section --------------------------------------
    auto* rl_group = new QGroupBox("API Rate Limit");
    auto* rl_layout = new QVBoxLayout(rl_group);

    auto info = auth.get_rate_limit();
    auto* rl_label = new QLabel;
    if (info.limit > 0) {
        QString text;
        text += QString("Remaining: <b>%1</b> / %2")
            .arg(info.remaining).arg(info.limit);
        if (info.reset > 0) {
            QDateTime dt = QDateTime::fromSecsSinceEpoch(info.reset);
            text += "<br>Resets: " + dt.toLocalTime().toString(Qt::TextDate);
        }
        if (info.last_updated > 0) {
            QDateTime lu = QDateTime::fromSecsSinceEpoch(info.last_updated);
            text += "<br>Last request: " + lu.toLocalTime().toString(Qt::TextDate);
        }
        rl_label->setText(text);
    } else {
        rl_label->setText("No API requests made yet in this session.");
    }
    rl_layout->addWidget(rl_label);
    layout->addWidget(rl_group);

    // -- Dialog buttons ------------------------------------------
    layout->addSpacing(8);
    auto* btn_box = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(btn_box, &QDialogButtonBox::rejected, &dlg, &QDialog::close);
    layout->addWidget(btn_box);

    // -- Button actions ------------------------------------------
    connect(save_btn, &QPushButton::clicked, [&]() {
        QString key = key_edit->text().trimmed();
        if (key.isEmpty()) {
            QMessageBox::warning(&dlg, "API Key",
                "Enter your Nexus Mods API key or click Clear to remove it.");
            return;
        }
        auth.set_api_key(key.toStdString());
        clear_btn->setEnabled(true);
        save_btn->setText("Update");
        engine::Logger::instance().info("Nexus API key saved");
        QMessageBox::information(&dlg, "API Key", "API key saved successfully.");
    });

    connect(clear_btn, &QPushButton::clicked, [&]() {
        auto reply = QMessageBox::question(&dlg, "Clear API Key",
            "Remove the stored Nexus Mods API key?",
            QMessageBox::Yes | QMessageBox::No);
        if (reply == QMessageBox::Yes) {
            auth.clear_api_key();
            key_edit->clear();
            clear_btn->setEnabled(false);
            save_btn->setText("Save");
            engine::Logger::instance().info("Nexus API key cleared");
        }
    });

    dlg.exec();
}

void MainWindow::show_instance_statistics() {
    if (current_instance_root_.empty()) {
        QMessageBox::information(this, "Instance Statistics",
                                 "No instance is currently loaded.");
        return;
    }

    auto cache_dir = current_instance_root_ / "cache";
    int total_mods = 0;
    for (const auto& m : mod_model_->mods()) {
        if (!m.is_separator && !m.is_overwrite) ++total_mods;
    }

    InstanceStatisticsDialog dlg(current_instance_root_, cache_dir,
                                 total_mods, this);
    dlg.exec();
}

void MainWindow::show_instance_switcher() {
    auto instances_dir = engine::default_instances_dir();

    InstanceSwitcherDialog dlg(plugin_loader_, this);
    dlg.load_instances(instances_dir.string());

    if (dlg.exec() != QDialog::Accepted) return;

    if (dlg.create_requested()) {
        std::vector<engine::DetectedGame> installed_games;
        if (plugin_loader_) {
            std::vector<std::pair<uint32_t, std::pair<std::string, std::string>>> game_specs;
            for (const auto& p : plugin_loader_->plugins()) {
                if (p.steam_appid > 0)
                    game_specs.push_back({p.steam_appid, {p.game_id, p.game_display_name}});
            }
            installed_games = engine::GameDetector::detect_steam_games_multi(game_specs);
        }

        std::vector<ui::GameEntry> installed_entries, available_entries;
        for (const auto& g : installed_games) {
            ui::GameEntry e;
            e.game_id = g.game_id;
            e.display_name = g.name;
            e.steam_appid = g.steam_appid;
            e.installed = true;
            e.install_path = g.install_path;
            installed_entries.push_back(e);
        }
        if (plugin_loader_) {
            for (const auto& p : plugin_loader_->plugins()) {
                bool found = false;
                for (const auto& ie : installed_entries) {
                    if (ie.game_id == p.game_id) { found = true; break; }
                }
                if (!found) {
                    ui::GameEntry e;
                    e.game_id = p.game_id;
                    e.display_name = p.game_display_name;
                    e.steam_appid = p.steam_appid;
                    e.installed = false;
                    available_entries.push_back(e);
                }
            }
        }

        QDialog sel_dlg(this);
        sel_dlg.setWindowTitle("Create New Instance");
        sel_dlg.setMinimumSize(600, 400);
        auto* layout = new QVBoxLayout(&sel_dlg);
        auto* selection = new ui::GameSelectionWidget(&sel_dlg);
        selection->set_games(installed_entries, available_entries);
        layout->addWidget(selection);

        ui::GameEntry chosen;
        bool chosen_ok = false;
        QObject::connect(selection, &ui::GameSelectionWidget::game_selected,
            [&](const ui::GameEntry& entry) {
                chosen = entry;
                chosen_ok = true;
                sel_dlg.accept();
            });

        if (sel_dlg.exec() != QDialog::Accepted || !chosen_ok) return;

        engine::DetectedGame dg;
        dg.game_id = chosen.game_id;
        dg.name = chosen.display_name;
        dg.steam_appid = chosen.steam_appid;
        dg.install_path = chosen.install_path;

        auto inst = engine::create_instance_for_game(dg, instances_dir);
        if (inst.info().game_id.empty()) {
            QMessageBox::warning(this, "Error",
                "Failed to create instance for " + QString::fromStdString(chosen.display_name));
            return;
        }

        engine::write_last_instance(inst.info().root.filename().string());
        set_game_info(chosen.game_id, chosen.display_name, "Default",
                      chosen.install_path, inst.info().root);
        engine::Logger::instance().debug("Created and switched to instance: " + inst.info().root.filename().string());
        return;
    }

    auto selected = dlg.selected_instance().toStdString();
    if (selected.empty()) return;

    auto inst = engine::Instance::installed(selected, instances_dir);
    if (!inst.read_toml()) {
        QMessageBox::warning(this, "Error",
            "Failed to read instance.toml for " + QString::fromStdString(selected));
        return;
    }

    auto& info = inst.info();
    std::string game_id = info.game_id;
    std::string display_name;
    if (plugin_loader_) {
        display_name = plugin_loader_->display_name_for(game_id);
    }
    if (display_name.empty()) display_name = game_id;

    engine::write_last_instance(selected);
    set_game_info(game_id, display_name, "Default", info.game_dir, inst.info().root);
    engine::Logger::instance().debug("Switched to instance: " + selected);
}

}  // namespace ui
