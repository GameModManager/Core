#include <algorithm>
#include <cstring>
#include <fstream>
#include <sys/wait.h>
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
#include "ui/smooth_scroll.h"
#include "engine/launcher.h"
#include "engine/fs_utils.h"
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
#include "engine/pipeline/extract_stage.h"
#include "engine/pipeline/fomod_stage.h"
#include "engine/pipeline/install_stage.h"
#include "engine/pipeline/plugin_claim_stage.h"
#include "engine/trace/trace_recorder.h"
#include "engine/deploy/strategy.h"
#include "runtime/runtime.h"
#include "ui/widgets/pipeline_window.h"

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
#include <QRegularExpression>
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
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDateTime>
#include <QDir>
#include <QLocale>
#include <QSettings>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <regex>
#include <sstream>
#include <thread>
#include <unordered_set>

namespace ui {

namespace {

// Extract .exe icon via wrestool, fallback to QFileIconProvider - same logic as ExecControlsBar
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

// Reap the subreaper supervisor forked by engine::launcher_game(). It never
// execs (stays "[gamemodmanager]"), so if the watchdog stops without
// waitpid()ing it, it remains a zombie forever. A cgroup-empty result means
// the game and its descendants are gone, so the supervisor exits as soon as
// its reap loop hits ECHILD; poll briefly so a stray reparented daemon can't
// hang the UI thread.
bool reap_supervisor(pid_t pid) {
    if (pid <= 0) return true;
    using namespace std::chrono;
    for (int attempt = 0; attempt < 20; ++attempt) {
        int status;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) return true;
        if (r < 0 && errno == ECHILD) return true;
        std::this_thread::sleep_for(milliseconds(100));
    }
    engine::Logger::instance().warn("Watchdog: supervisor " + std::to_string(pid) +
        " not reaped after 2s (stray child?)");
    return false;
}

}  // anonymous namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent) {
    setWindowTitle(tr("GameModManager"));
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

    // --- Left panel: profile bar, mod list, filter bar stacked vertically ---
    auto* left_panel = new QWidget(this);
    auto* left_layout = new QVBoxLayout(left_panel);
    left_layout->setContentsMargins(0, 0, 0, 0);
    left_layout->setSpacing(0);

    // Profile bar sits at top of left panel (inside same box as the mod list)
    profile_bar_ = new ProfileBar(this);
    left_layout->addWidget(profile_bar_);

    connect(profile_bar_, &ProfileBar::create_separator_clicked, this, [this]() {
        create_separator();
    });
    connect(profile_bar_, &ProfileBar::create_empty_mod_clicked, this, [this]() {
        create_empty_mod();
    });

    connect(profile_bar_, &ProfileBar::profile_changed, this, [this](const QString& profile) {
        current_profile_name_ = profile.toStdString();
        update_title();
    });

    connect(profile_bar_, &ProfileBar::import_clicked, this, [this]() {
        import_modlist();
    });
    connect(profile_bar_, &ProfileBar::export_modlist_clicked, this, [this]() {
        export_modlist();
    });
    connect(profile_bar_, &ProfileBar::import_modlist_clicked, this, [this]() {
        import_modlist();
    });

    mod_model_ = new ModListModel(this);
    mod_view_ = new ModTableView(this);
    mod_model_->set_view(mod_view_);
    mod_view_->setModel(mod_model_);

    // Highlight conflicting mods on selection + populate ConflictsTab
    connect(mod_view_->selectionModel(), &QItemSelectionModel::currentChanged,
            this, [this](const QModelIndex& current, const QModelIndex& /*previous*/) {
        if (!current.isValid()) {
            mod_model_->set_selected_mod({});
            auto* ct = right_panel_->conflicts_tab();
            if (ct) ct->clear_content();
            return;
        }
        const auto& mods = mod_model_->mods();
        if (current.row() >= 0 && current.row() < mods.size() &&
            !mods[current.row()].is_separator && !mods[current.row()].is_overwrite) {
            auto& selected = mods[current.row()];
            mod_model_->set_selected_mod(selected.id);

            // Push conflict data to the ConflictsTab
            auto* ct = right_panel_->conflicts_tab();
            if (ct) {
                ct->show_conflicts(selected.id, mods,
                                   last_conflict_registry_,
                                   mod_model_->conflict_pairs(),
                                   mod_model_->is_conflict_order_reversed());
            }
        } else {
            mod_model_->set_selected_mod({});
            auto* ct = right_panel_->conflicts_tab();
            if (ct) ct->clear_content();
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
        if (loading_) return;
        save_order();
        sync_separator_ids();
        apply_mod_filter();
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
        import_archives(paths);
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

    connect(filter_bar_, &ModFilterBar::filter_changed, this, [this]() {
        apply_mod_filter();
    });
    connect(filter_bar_, &ModFilterBar::group_changed, this, [this]() {
        apply_mod_filter();
    });
    connect(filter_bar_, &ModFilterBar::expand_all_clicked, this, [this]() {
        for (int i = 0; i < mod_model_->mods().size(); ++i)
            mod_model_->set_folded(i, false);
        apply_mod_filter();
    });
    connect(filter_bar_, &ModFilterBar::collapse_all_clicked, this, [this]() {
        for (int i = 0; i < mod_model_->mods().size(); ++i)
            mod_model_->set_folded(i, true);
        apply_mod_filter();
    });

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
    create_game_lock_overlay();

    // --- Status bar ---
    status_bar_ = new GmmStatusBar(this);
    statusBar()->addWidget(status_bar_, 1);

    // Global event filter for Konami code (child widgets may eat arrow keys)
    QApplication::instance()->installEventFilter(this);
    setFocusPolicy(Qt::StrongFocus);

    pipeline_thread_ = new PipelineThread(this);
    pipeline_thread_->start();

    // Download finished (download-only, MO2 model): the row becomes Complete
    // with a real file path; installation is a separate user-triggered step.
    connect(pipeline_thread_->worker(), &PipelineWorker::download_complete,
            this, [this](const std::string& id, bool success,
                         const std::string& archive_path, const std::string& name) {
        auto* dt = right_panel_->downloads_tab();
        if (dt) {
            if (!archive_path.empty()) dt->set_file_path(id, archive_path);
            // Replace the "Mod #<id> - file <id>" placeholder with the real
            // name the provider resolved (empty = nothing available).
            if (!name.empty()) dt->rename_download(id, name);
            dt->mark_complete(id, success);
        }
        // Persist download state
        save_download_manifest();
    });

    // Install finished (user-triggered via the Downloads context menu or
    // double-click): reload the mod list and mark the entry Installed.
    connect(pipeline_thread_->worker(), &PipelineWorker::install_complete,
            this, [this](const std::string& mod_id, bool success, const std::string&) {
        auto* dt = right_panel_->downloads_tab();
        if (dt) {
            if (success) {
                if (!current_game_id_.empty()) {
                    engine::Logger::instance().debug(
                        "Install finished for " + mod_id + ", reloading mods...");
                    load_mods_from_game();
                }
                dt->mark_installed(mod_id);
            } else {
                dt->mark_complete(mod_id, false);
            }
        }
        // Persist download state
        save_download_manifest();
    });

    // A download was paused mid-fetch (partial file kept for resume).
    connect(pipeline_thread_->worker(), &PipelineWorker::paused,
            this, [this](const std::string& id) {
        auto* dt = right_panel_->downloads_tab();
        if (dt) dt->mark_paused(id);
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

    connect(right_panel_->exec_controls(), &ExecControlsBar::add_entry_requested,
            this, &MainWindow::on_add_entry_requested);

    // Keep the persisted per-instance selection in sync with the live combo
    connect(right_panel_->exec_controls(), &ExecControlsBar::current_executable_changed,
            this, [this]() {
        pending_exec_selection_ =
            right_panel_->exec_controls()->current_executable();
    });

    // Start IPC server to receive nxm:// URLs from other GMM processes
    nxm_ipc_ = new engine::NxmIpcServer(this);
    if (nxm_ipc_->startListening()) {
        connect(nxm_ipc_, &engine::NxmIpcServer::nxmUrlReceived,
                this, [this](const QString& url) {
            std::string raw = url.toStdString();
            // Accept gmm:// URLs too - convert to nxm:// for the parser
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

    // Populate Recent Instances submenu
    refresh_recent_instances();

    // Smooth scrolling on all item views (mod list, right-panel tables).
    // TODO: gate behind a Settings "Smooth scrolling" checkbox.
    ui::enable_smooth_scrolling(this);
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
    nxm_links_.clear();  // NXM links are instance-scoped

    current_game_id_ = game_id;
    current_game_name_ = game_display_name;
    current_profile_name_ = profile_name;
    current_game_dir_ = game_dir;
    current_instance_root_ = instance_root;
    if (!instance_root.empty())
        conflict_cache_path_ = instance_root / "cache" / "conflict_cache.json";
    // Restore the persisted per-instance executable selection BEFORE the
    // combo is populated, so set_executables can land on it instead of
    // defaulting to the first real entry.
    restore_exec_selection();
    update_title();

    // (Loaded plugin list is logged once by PluginLoader::load_directory)

    if (!game_dir.empty() && knowledge_) {
        update_status_bar_for_game();

        // Rebuild right-panel tabs for this game
        if (plugin_loader_)
            right_panel_->set_capabilities(&plugin_loader_->capabilities());
        right_panel_->set_game(current_game_id_);

        // Connect conflicts tab signals (tab created during set_game)
        auto* ct = right_panel_->conflicts_tab();
        if (ct) {
            connect(ct, &ui::ConflictsTab::image_diff_requested,
                    this, &MainWindow::on_image_diff_requested);
        }

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
        // Metadata format inside installed mod folders: MO2's meta.ini by
        // default, the game's XML file (Isaac's metadata.xml) if the game
        // registered the metadata_file hook.
        ctx.metadata_file = knowledge_->get(current_game_id_, "metadata_file", "meta.ini");

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

        // Build the install pipeline from the 3-stage template.  A plugin
        // stage claim (StageRegistry) wins over the core implementation for
        // the same stage name; stages with no implementation at all stay out
        // of the pipeline and render as "Not implemented" in the pipeline
        // window.  Installs are intentionally download-free: they unpack the
        // already-downloaded archive and copy it into the mods folder, with a
        // FOMOD-detection branch that aborts with a warning until FOMOD
        // installers are supported.
        auto claim_for = [this](const std::string& stage_name)
            -> std::optional<engine::StageClaim> {
            if (!plugin_loader_) return std::nullopt;
            std::optional<engine::StageClaim> best;
            for (const auto& c : plugin_loader_->stage_registry().claims()) {
                if (c.game_id == current_game_id_ && c.stage_name == stage_name) {
                    if (!best || c.priority > best->priority) best = c;
                }
            }
            return best;
        };

        std::vector<std::pair<const char*, std::function<std::unique_ptr<engine::Stage>()>>>
            core_makers = {
                {"Extract", [] { return std::make_unique<engine::ExtractStage>(); }},
                {"Fomod",   [] { return std::make_unique<engine::FomodStage>(); }},
                {"Install", [] { return std::make_unique<engine::InstallStage>(); }},
            };
        static const char* kInstallStages[] = {
            "Extract", "Fomod", "Install",
        };

        auto pipeline = std::make_unique<engine::Pipeline>();
        pipeline->set_context(ctx);
        pipeline->set_flow_id("install");

        std::vector<engine::TraceStage> flow_stages;
        flow_stages.reserve(3);
        for (const char* stage_name : kInstallStages) {
            auto claim = claim_for(stage_name);
            std::unique_ptr<engine::Stage> impl;
            std::string origin = "core";

            if (claim) {
                impl = std::make_unique<engine::PluginClaimStage>(
                    stage_name, claim->plugin_id, claim->handler);
                origin = claim->plugin_id;
            } else {
                for (const auto& [name, maker] : core_makers) {
                    if (std::strcmp(name, stage_name) == 0) {
                        impl = maker();
                        break;
                    }
                }
            }

            engine::TraceStage ts;
            ts.name = stage_name;
            ts.origin = origin;
            ts.implemented = (impl != nullptr);
            if (impl) ts.description = impl->description();
            flow_stages.push_back(std::move(ts));
            if (impl) pipeline->add_stage(std::move(impl));
        }
        engine::TraceRecorder::instance().declare_flow(
            "install", "Mod install pipeline", std::move(flow_stages));

        // The download flow is fetch-only (downloads are decoupled from
        // installs); PipelineWorker::download_mod runs it.
        engine::TraceRecorder::instance().declare_flow("download", "Mod download", {
            {"Fetch", "core",
             "Downloads the archive into the instance downloads dir "
             "(pause/resume supported)"},
        });

        // Declare the sort + launch flows eagerly too - the pipeline window
        // must show the full stage list (and who provides what) before either
        // flow has ever run.  sort_mods()/launch_with_executable() only
        // begin_flow() at run time.
        engine::TraceRecorder::instance().declare_flow("sort", "Auto-sort", {
            {"Gather mod info", "core",
             "Collects folder names, display names and workshop IDs from the mod list"},
            {"Run sort provider", "core",
             "Invokes the game's registered sort provider"},
            {"Apply order", "core",
             "Reorders the mod list per the provider's result"},
            {"Save order", "core",
             "Writes the new load order to disk"},
        });
        engine::TraceRecorder::instance().declare_flow("launch", "Game launch", {
            {"Sync disk order", "core",
             "Writes the UI's load order to disk before launch"},
            {"Prepare launch environment", "core",
             "Sets up the overlay / Proton environment and deploys enabled mods"},
            {"Launch executable", "core",
             "Starts the game through the launch tier chain"},
            {"Monitor process", "core",
             "Watches the running game and captures writes on exit"},
        });

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
        dt->set_downloads_dir(current_instance_root_ / "downloads");
        connect(dt, &DownloadsTab::install_requested,
                this, [this](const std::string& mod_id, const std::filesystem::path& fp,
                             const std::string& source_type, const std::string& source_id,
                             int file_id, const std::string& display_name) {
            if (!pipeline_thread_) return;
            QMetaObject::invokeMethod(pipeline_thread_->worker(),
                [this, mod_id, fp, source_type, source_id, file_id, display_name]() {
                pipeline_thread_->worker()->install_mod(
                    mod_id, fp.string(), source_type, source_id, file_id, display_name);
            }, Qt::QueuedConnection);
        });
        connect(dt, &DownloadsTab::pause_requested,
                this, [this](const std::string& id) {
            if (!pipeline_thread_) return;
            QMetaObject::invokeMethod(pipeline_thread_->worker(), [this, id]() {
                pipeline_thread_->worker()->pause_download(id);
            }, Qt::QueuedConnection);
        });
        connect(dt, &DownloadsTab::resume_requested,
                this, [this](const std::string& id) {
            auto it = nxm_links_.find(id);
            if (it == nxm_links_.end() || !pipeline_thread_) return;
            auto* dtab = right_panel_->downloads_tab();
            if (dtab) dtab->mark_downloading(id);
            auto link = it->second;
            auto mods_dir = mods_dir_path().string();
            auto meta_dir = current_instance_root_.empty()
                ? "" : (current_instance_root_ / "meta").string();
            QMetaObject::invokeMethod(pipeline_thread_->worker(),
                [this, id, link, mods_dir, meta_dir]() {
                pipeline_thread_->worker()->download_mod(
                    id, link, current_game_id_, mods_dir, meta_dir);
            }, Qt::QueuedConnection);
        });
        connect(dt, &DownloadsTab::entry_removed,
                this, [this](const std::string& id) {
                nxm_links_.erase(id);
                save_download_manifest();
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

    refresh_recent_instances();

    // Self-heal the OS-level nxm:// handler once per session, after the window
    // is shown (deferred so the modal doesn't appear mid-construction).
    QTimer::singleShot(0, this, &MainWindow::ensure_nxm_handler_default);
}

void MainWindow::update_title() {
    if (current_game_name_.empty()) {
        setWindowTitle(tr("GameModManager"));
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
        show_instance_switcher();
    });
    connect(menu_bar_, &AppMenuBar::open_instance_requested, this, [this]() {
        show_instance_switcher();
    });
    connect(menu_bar_, &AppMenuBar::recent_instance_selected, this, [this](const QString& name) {
        switch_to_instance(name);
    });
    connect(menu_bar_, &AppMenuBar::import_mods_requested, this, [this]() {
        if (current_instance_root_.empty()) return;
        const QStringList paths = QFileDialog::getOpenFileNames(this,
            tr("Import Mod Archives"), QString(),
            tr("Archives (*.zip *.7z *.rar *.tar *.gz);;All files (*)"));
        if (paths.isEmpty()) return;
        import_archives(paths);
    });
    connect(menu_bar_, &AppMenuBar::export_mods_requested, this, [this]() {
        export_modlist();
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
        priority_move_selected(-1);
    });
    connect(menu_bar_, &AppMenuBar::priority_down_requested, this, [this]() {
        priority_move_selected(+1);
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
    connect(menu_bar_, &AppMenuBar::pipeline_requested, this,
            &MainWindow::show_pipeline_window);
    connect(status_bar_, &GmmStatusBar::pipeline_clicked, this,
            &MainWindow::show_pipeline_window);
    connect(menu_bar_, &AppMenuBar::refresh_requested, this, [this]() {
        if (current_game_id_.empty()) return;
        load_mods_from_game();
    });

    connect(menu_bar_, &AppMenuBar::icon_size_requested, this, [this](int size) {
        icon_size_ = size;
        toolbar_area_->setIconSize(QSize(size, size));
        toolbar_->set_icon_size(size);
    });

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
        QMessageBox::about(this, tr("About GameModManager"),
            "<h3>GameModManager</h3>"
            "<p>Version " VERSION "</p>"
            "<p>" + tr("Cross-platform game mod manager with multi-repo plugin support.") + "</p>");
    });
    connect(menu_bar_, &AppMenuBar::about_qt_requested, this, [this]() {
        QMessageBox::aboutQt(this, tr("About Qt"));
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

    // Game is running - queue the change instead of writing to disk
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

    auto mod_folder = resolve_mod_folder(mod_id.toStdString(), mods_subpath);

    if (enabled) {
        (void)engine::ModScanner::enable_mod(*knowledge_, current_game_id_, mod_folder);
    } else {
        (void)engine::ModScanner::disable_mod(*knowledge_, current_game_id_, mod_folder);
    }
}

void MainWindow::sync_priorities() {
    if (loading_) return;
    if (!knowledge_ || current_game_id_.empty() || current_game_dir_.empty()) return;

    // Game is running - skip disk write; full order saved at flush
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
        // Write game-native priority - resolve actual mod folder location.
        // Only games that encode priority into mod-folder metadata (Isaac's
        // NNN prefix in metadata.xml, read by the game itself) get a folder
        // write; MO2-style games persist priority in the meta dir sidecar
        // above and read load order from their plugins.txt / order encoding.
        if (!mods[i].is_overwrite && !mods[i].is_separator && !mods_subpath.empty()) {
            auto metadata_file = knowledge_->get(current_game_id_, "metadata_file", "meta.ini");
            if (!metadata_file.empty() && metadata_file != "meta.ini") {
                auto mod_folder = resolve_mod_folder(mods[i].id.toStdString(), mods_subpath);
                (void)engine::ModScanner::set_priority(*knowledge_, current_game_id_, mod_folder, i);
            }
        }
    }
}

void MainWindow::sort_mods() {
    auto& trace = engine::TraceRecorder::instance();
    trace.begin_flow("sort");

    auto* provider = engine::SortRegistry::instance().get_provider(current_game_id_);
    if (!provider) {
        engine::Logger::instance().warn("No sort provider registered for game: " + current_game_id_);
        trace.end_flow("sort", false, "No sort provider for " + current_game_id_);
        return;
    }

    // Build mod info list from current model
    trace.begin_stage("sort", "Gather mod info");
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
    trace.end_stage("sort", true, std::to_string(mod_infos.size()) + " mod(s) collected");

    // Call the sort provider
    trace.begin_stage("sort", "Run sort provider");
    auto result = provider->sort(mod_infos);
    trace.end_stage("sort", true, std::string("Provider: ") + provider->name());

    // Apply the sorted order to the model
    trace.begin_stage("sort", "Apply order");
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
    trace.end_stage("sort", true, "New order applied");

    trace.begin_stage("sort", "Save order");
    save_order();
    trace.end_stage("sort", true, "Order persisted");

    engine::Logger::instance().debug("Mods sorted by " + std::string(provider->name()));
    trace.end_flow("sort", true);
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
            // Merge - instance mods override game-native mods with same folder name
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

    // Clear all existing mods (including game-native) - needed for instance switching
    mod_model_->remove_all_mods();

    // MERGED pseudo-mod is game-dependent (Isaac only); turn the flag on/off
    // before anything adds rows, so switching Isaac <-> Skyrim adds/removes it.
    auto uses_merged = knowledge_->get(current_game_id_, "uses_merged", "");
    mod_model_->set_uses_merged(uses_merged == "true");

    // Filter out MERGED pseudo-mod folder from scan results
    scanned.erase(std::remove_if(scanned.begin(), scanned.end(),
        [](const engine::ScannedMod& m) { return m.folder_name == "MERGED"; }),
        scanned.end());

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

    // Read persisted priority from meta.ini for ALL entries (including separators, Overwrite).
    // Mods without a persisted priority (e.g. freshly installed) get the bottom of the user
    // band - MO2's rule: a new mod gets the highest regular priority, just above the pinned
    // Overwrite/MERGED rows. set_priority() only writes the field; load_order() applies it.
    {
        auto meta_dir = meta_dir_path();
        if (!meta_dir.empty()) {
            int regular_rows = 0;
            for (const auto& m : mod_model_->mods()) {
                if (!m.is_overwrite && !m.is_merged) ++regular_rows;
            }
            int bottom_priority = std::max(0, regular_rows - 1);
            for (const auto& m : mod_model_->mods()) {
                auto meta = engine::ModMeta::load(meta_dir, m.id.toStdString());
                int p = meta.priority();
                if (p < 0) p = bottom_priority;
                mod_model_->set_priority(m.id, p);
            }
        }
    }

    // Ensure MERGED pseudo-mod is present (after loading scanned mods, before sorting)
    mod_model_->ensure_merged_present();

    loading_ = false;

    // Sort by priority to restore saved order
    load_order();

    // Persist priorities to {modname}.ini - including the ones just assigned to
    // freshly installed mods above - so the order survives restarts. This also
    // fires when load_order() produced no reorder (already-correct order).
    sync_priorities();

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

std::filesystem::path MainWindow::resolve_mod_folder(const std::string& mod_id, const std::string& mods_subpath) const {
    auto folder = mods_dir_path() / mod_id;
    if (std::filesystem::exists(folder))
        return folder;
    if (!mods_subpath.empty()) {
        auto fallback = current_game_dir_ / mods_subpath / mod_id;
        if (std::filesystem::exists(fallback))
            return fallback;
    }
    return folder;  // return the instance path even if it doesn't exist
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

    // Workshop ID pattern - used to detect Steam Workshop mods from folder names
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
            // No meta file exists - create a default one (already at CURRENT_META_VERSION)
            meta = engine::ModMeta::from_default(folder_name, "manual", "");
            meta.save(meta_dir, folder_name);

        } else {
            // Existing meta - check if upgrade is needed
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
    if (!knowledge_ || current_game_id_.empty() || current_game_dir_.empty()) {
        refresh_data_tab();
        return;
    }

    // Read per-game config from knowledge hooks (needed before mod_infos for overwrite priority)
    auto conflict_extensions = knowledge_->get(current_game_id_, "conflict_extensions", "");
    auto ignored_files = knowledge_->get(current_game_id_, "ignored_files", "");
    // Mod folders carry per-mod metadata files the manager itself writes
    // (meta.ini) or that the game reads (metadata.xml / disable marker).
    // Every mod folder has them, so exclude them from conflict counting.
    auto metadata_file = knowledge_->get(current_game_id_, "metadata_file", "meta.ini");
    auto disable_file = knowledge_->get(current_game_id_, "disable_mechanism", "");
    for (const auto* f : {&metadata_file, &disable_file}) {
        if (f->empty()) continue;
        if (ignored_files.find(*f) != std::string::npos) continue;
        if (!ignored_files.empty()) ignored_files += ",";
        ignored_files += *f;
    }
    auto conflict_reversed = knowledge_->get(current_game_id_, "conflict_order_reversed", "") == "true";
    auto conflict_scan_dirs = knowledge_->get(current_game_id_, "conflict_scan_dirs", "");

    // Collect mod info - only enabled mods affect the game
    std::vector<engine::ConflictEngine::ModInfo> mod_infos;
    for (const auto& mod : mod_model_->mods()) {
        if (mod.is_separator) continue;
        if (!mod.enabled && !mod.is_overwrite && !mod.is_merged) continue;
        if (mod.is_overwrite) {
            int ow_priority = conflict_reversed ? -1 : 999999;
            mod_infos.emplace_back(mod.id.toStdString(), ow_priority);
            continue;
        }
        if (mod.is_merged) {
            int mg_priority = conflict_reversed ? 0 : 999998;
            mod_infos.emplace_back(mod.id.toStdString(), mg_priority);
            continue;
        }
        mod_infos.emplace_back(mod.id.toStdString(), mod.priority);
    }

    if (mod_infos.empty()) {
        last_conflict_registry_.clear();
        refresh_data_tab();
        return;
    }

    auto mods_dir = mods_dir_path();

    // Determine game's native mods directory (for mods that live there instead of instance)
    std::filesystem::path game_mods_dir;
    if (!current_game_dir_.empty() && knowledge_) {
        auto game_mods_subpath = knowledge_->get(current_game_id_, "mods_subpath", "");
        game_mods_dir = current_game_dir_;
        if (!game_mods_subpath.empty())
            game_mods_dir /= game_mods_subpath;
        // Only pass as extra dir if it differs from the instance mods dir
        if (game_mods_dir == mods_dir)
            game_mods_dir.clear();
    }

    engine::ConflictEngine engine;
    auto stats = engine.compute(mods_dir, mod_infos,
                                 conflict_extensions, ignored_files,
                                 conflict_reversed, conflict_cache_path_,
                                 game_mods_dir, conflict_scan_dirs);

    // Push results into the model
    for (const auto& [folder_name, cs] : stats) {
        mod_model_->set_conflict_stats(QString::fromStdString(folder_name), cs.wins, cs.losses);
    }
    // Zero out any stale stats for disabled mods (not fed to the engine)
    for (const auto& mod : mod_model_->mods()) {
        if (!mod.enabled && !mod.is_overwrite && !mod.is_merged && !mod.is_separator)
            mod_model_->set_conflict_stats(mod.id, 0, 0);
    }

    // "Redundant" mods: every file they provide is won by a higher-priority
    // owner, so nothing the mod provides actually takes effect.
    const auto& registry = engine.last_registry();
    std::unordered_set<std::string> owns_files;
    std::unordered_set<std::string> wins_a_file;
    for (const auto& [path, owners] : registry) {
        if (owners.empty()) continue;
        for (const auto& [owner, _] : owners) owns_files.insert(owner);
        const auto& winner = conflict_reversed
            ? *std::min_element(owners.begin(), owners.end(),
                                [](const auto& a, const auto& b) { return a.second < b.second; })
            : *std::max_element(owners.begin(), owners.end(),
                                [](const auto& a, const auto& b) { return a.second < b.second; });
        wins_a_file.insert(winner.first);
    }
    for (const auto& mod : mod_model_->mods()) {
        bool redundant = owns_files.count(mod.id.toStdString()) > 0 &&
                         wins_a_file.count(mod.id.toStdString()) == 0;
        mod_model_->set_conflict_redundant(mod.id, redundant);
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
    last_conflict_registry_ = engine.last_registry();
    refresh_data_tab();
}

void MainWindow::refresh_data_tab() {
    auto* dt = right_panel_->data_tab();
    if (!dt) return;

    if (last_conflict_registry_.empty() || current_game_id_.empty()) {
        dt->clear_content();
        return;
    }

    auto mods_dir = mods_dir_path();

    // Game-native mods dir (may equal mods_dir -> then fallback is unused)
    std::filesystem::path game_mods_dir;
    if (!current_game_dir_.empty() && knowledge_) {
        auto game_mods_subpath = knowledge_->get(current_game_id_, "mods_subpath", "");
        game_mods_dir = current_game_dir_;
        if (!game_mods_subpath.empty())
            game_mods_dir /= game_mods_subpath;
        if (game_mods_dir == mods_dir)
            game_mods_dir.clear();
    }

    dt->show_data(last_conflict_registry_, mod_model_->mods(),
                  mod_model_->is_conflict_order_reversed(),
                  mods_dir, game_mods_dir);
}

void MainWindow::on_image_diff_requested(const QString& relative_path) {
    if (!plugin_loader_ || !plugin_loader_->has_image_diff())
        return;

    // Collect all mods that own this file from the conflict registry
    auto it = last_conflict_registry_.find(relative_path.toStdString());
    if (it == last_conflict_registry_.end() || it->second.size() < 2)
        return;

    const auto& owners = it->second;
    std::vector<std::string> source_paths;
    source_paths.reserve(owners.size());

    auto mods_dir = mods_dir_path();
    std::filesystem::path game_mods_dir;
    if (!current_game_dir_.empty() && knowledge_) {
        auto subpath = knowledge_->get(current_game_id_, "mods_subpath", "");
        game_mods_dir = current_game_dir_;
        if (!subpath.empty()) game_mods_dir /= subpath;
    }

    for (const auto& [mod_name, _] : owners) {
        // Check instance mods dir first, then game native mods dir
        std::filesystem::path abs_path = mods_dir / mod_name / relative_path.toStdString();
        if (std::filesystem::exists(abs_path)) {
            source_paths.push_back(abs_path.string());
            continue;
        }
        if (!game_mods_dir.empty()) {
            abs_path = game_mods_dir / mod_name / relative_path.toStdString();
            if (std::filesystem::exists(abs_path)) {
                source_paths.push_back(abs_path.string());
                continue;
            }
        }
    }

    if (source_paths.size() < 2) return;

    // Compute output path - write to MERGED pseudo-mod folder
    std::filesystem::path output_path = mods_dir_path() / "MERGED" / relative_path.toStdString();
    std::error_code ec;
    std::filesystem::create_directories(output_path.parent_path(), ec);

    // Invoke the image diff provider
    const auto& provider = plugin_loader_->image_diff_provider();
    if (provider.fn) {
        std::vector<const char*> c_paths;
        c_paths.reserve(source_paths.size());
        for (const auto& p : source_paths)
            c_paths.push_back(p.c_str());

        std::string out_str = output_path.string();
        provider.fn(c_paths.data(), c_paths.size(), out_str.c_str(), provider.user_data);
    }
}

void MainWindow::setup_mod_list_context_menu() {
    mod_view_->setContextMenuPolicy(Qt::CustomContextMenu);

    connect(mod_view_, &QWidget::customContextMenuRequested,
            this, [this](const QPoint& pos) {
        auto idx = mod_view_->indexAt(pos);
        if (!idx.isValid()) return;

        int row = idx.row();
        if (row < 0 || row >= mod_model_->mods().size()) return;
        const auto& entry = mod_model_->mods()[row];

        QMenu menu;

        if (entry.is_overwrite) {
            menu.addAction(QIcon::fromTheme("edit-clear"), tr("Clear Overwrite"),
                this, [this]() { clear_overwrite(); });
            menu.addAction(QIcon::fromTheme("document-new"), tr("Create Mod from Overwrite"),
                this, [this]() { create_mod_from_overwrite(); });
            menu.exec(mod_view_->viewport()->mapToGlobal(pos));
            return;
        }

        if (entry.is_separator) {
            menu.addAction(QIcon::fromTheme("document-edit"), tr("Edit"),
                this, [this, row]() { edit_separator(row); });
            menu.addAction(QIcon::fromTheme("edit-delete"), tr("Delete"),
                this, [this, row]() { delete_separator(row); });
            menu.exec(mod_view_->viewport()->mapToGlobal(pos));
            return;
        }

        // --- Mod rows below ---
        auto sel = mod_view_->selectionModel()->selectedRows();
        bool multi = sel.size() > 1;

        if (multi) {
            menu.addAction(QIcon::fromTheme("dialog-ok"), tr("Enable Selected"),
                this, [this]() { toggle_selected_mods(true); });
            menu.addAction(QIcon::fromTheme("dialog-cancel"), tr("Disable Selected"),
                this, [this]() { toggle_selected_mods(false); });
            menu.addSeparator();
            menu.addAction(QIcon::fromTheme("edit-delete"), tr("Remove"),
                this, [this]() { remove_selected_mods(); });
            menu.exec(mod_view_->viewport()->mapToGlobal(pos));
            return;
        }

        // Single mod - full menu
        auto mod_id = entry.id;
        bool has_conflicts = entry.conflict_wins + entry.conflict_losses > 0;

        // Change Separator submenu
        auto* sep_submenu = menu.addMenu(QIcon::fromTheme("view-sort"), tr("Change Separator"));
        bool any_seps = false;
        for (const auto& m : mod_model_->mods()) {
            if (m.is_separator) {
                any_seps = true;
                sep_submenu->addAction(m.name, this, [this, mod_id, id = m.id]() {
                    move_to_separator(mod_id, id);
                });
            }
        }
        sep_submenu->setEnabled(any_seps);

        menu.addAction(QIcon::fromTheme("list-add"), tr("Create Separator"),
            this, [this, row]() { create_separator_at_row(row); });

        if (has_conflicts) {
            menu.addSeparator();
            menu.addAction(QIcon::fromTheme("go-top"), tr("Send to Highest Priority"),
                this, [this, mod_id]() { send_to_highest_priority(mod_id); });
            menu.addAction(QIcon::fromTheme("go-bottom"), tr("Send to Lowest Priority"),
                this, [this, mod_id]() { send_to_lowest_priority(mod_id); });

            if (!entry.separator_id.isEmpty() && mod_model_->has_conflicts_within_separator(mod_id)) {
                menu.addAction(QIcon::fromTheme("go-up"), tr("Send to Highest in Separator"),
                    this, [this, mod_id]() { send_to_highest_in_separator(mod_id); });
                menu.addAction(QIcon::fromTheme("go-down"), tr("Send to Lowest in Separator"),
                    this, [this, mod_id]() { send_to_lowest_in_separator(mod_id); });
            }
        }

        menu.addSeparator();
        menu.addAction(QIcon::fromTheme("dialog-ok"), tr("Enable Selected"),
            this, [this]() { toggle_selected_mods(true); });
        menu.addAction(QIcon::fromTheme("dialog-cancel"), tr("Disable Selected"),
            this, [this]() { toggle_selected_mods(false); });

        menu.addSeparator();
        menu.addAction(QIcon::fromTheme("document-edit"), tr("Rename Mod..."),
            this, [this]() { rename_selected_mod(); });

        menu.addSeparator();
        if (!entry.source_type.isEmpty()) {
            auto src = source_visit_info(entry.source_type, entry.source_id);
            if (!src.label.isEmpty()) {
                auto* visit_act = menu.addAction(QIcon::fromTheme("text-html"), src.label,
                    this, [this, src]() {
                        if (!src.url.isEmpty())
                            QDesktopServices::openUrl(QUrl(src.url));
                    });
                visit_act->setEnabled(!src.url.isEmpty());
            }
        }
        menu.addAction(QIcon::fromTheme("folder"), tr("Open in File Manager"),
            this, [this, mod_id]() {
                auto folder = mods_dir_path() / mod_id.toStdString();
                std::error_code ec;
                if (!std::filesystem::exists(folder, ec)) {
                    // Fall back to game's native mods directory
                    auto game_mods_subpath = knowledge_
                        ? knowledge_->get(current_game_id_, "mods_subpath", "") : "";
                    auto fallback = current_game_dir_;
                    if (!game_mods_subpath.empty())
                        fallback /= game_mods_subpath;
                    folder = fallback / mod_id.toStdString();
                }
                QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(folder.string())));
            });

        menu.addSeparator();
        menu.addAction(QIcon::fromTheme("edit-delete"), tr("Remove"),
            this, [this]() { remove_selected_mods(); });

        menu.exec(mod_view_->viewport()->mapToGlobal(pos));
    });
}

void MainWindow::clear_overwrite() {
    auto reply = QMessageBox::question(this, tr("Clear Overwrite"),
        tr("Remove all files from the Overwrite folder? This cannot be undone."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    if (!current_instance_root_.empty()) {
        auto overwrite_dir = current_instance_root_ / "overwrite";
        if (engine::SyncStage::clear_overwrite(overwrite_dir)) {
            engine::Logger::instance().debug("Overwrite cleared");
            QMessageBox::information(this, tr("Overwrite"), tr("Overwrite folder cleared."));
        } else {
            QMessageBox::warning(this, tr("Overwrite"), tr("Failed to clear Overwrite folder."));
        }
    }
}

void MainWindow::create_mod_from_overwrite() {
    bool ok;
    auto name = QInputDialog::getText(this, tr("Create Mod from Overwrite"),
        tr("Mod name:"), QLineEdit::Normal, QString(), &ok);
    if (!ok || name.isEmpty()) return;
    if (current_instance_root_.empty()) return;

    auto overwrite_dir = current_instance_root_ / "overwrite";
    auto mods_subpath = knowledge_ ? knowledge_->get(current_game_id_, "mods_subpath", "") : std::string();
    if (mods_subpath.empty()) return;

    auto mod_dir = mods_dir_path() / name.toStdString();

    std::vector<std::string> rel_paths;
    std::error_code ec;
    if (std::filesystem::exists(overwrite_dir)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(overwrite_dir)) {
            if (entry.is_regular_file()) {
                auto rel = std::filesystem::relative(entry.path(), overwrite_dir, ec);
                if (!ec) rel_paths.push_back(rel.string());
            }
        }
    }
    if (rel_paths.empty()) {
        QMessageBox::information(this, tr("Create Mod"), tr("Overwrite folder is empty."));
        return;
    }

    if (engine::SyncStage::promote_to_mod(overwrite_dir, mod_dir, rel_paths)) {
        // Write the game's metadata file so ModScanner picks the mod up.
        auto metadata_file = knowledge_->get(current_game_id_, "metadata_file", "meta.ini");
        engine::ModMeta::write_game_metadata(mod_dir, metadata_file,
                                             name.toStdString(), "1.0", "0");
        auto id = name;
        mod_model_->add_mod(id, name, "");
        engine::Logger::instance().debug("Promote Overwrite to mod: " + name.toStdString());
        QMessageBox::information(this, tr("Create Mod"), tr("Overwrite contents promoted to mod: %1").arg(name));
    } else {
        QMessageBox::warning(this, tr("Create Mod"), tr("Failed to promote Overwrite files."));
    }
}

void MainWindow::remove_selected_mods() {
    auto sel = mod_view_->selectionModel()->selectedRows();
    if (sel.isEmpty()) return;

    QStringList names;
    for (const auto& idx : sel) {
        int r = idx.row();
        if (r < 0 || r >= mod_model_->mods().size()) continue;
        if (mod_model_->mods()[r].is_overwrite) continue;
        names.append(mod_model_->mods()[r].name);
    }
    if (names.isEmpty()) return;

    auto reply = QMessageBox::question(this, tr("Remove Mods"),
        tr("Move %1 mod(s) to the trash bin?\n\n%2\n\nTheir files stay in the system trash and can be restored.")
            .arg(names.size()).arg(names.join("\n")),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    auto mods_subpath = knowledge_ ? knowledge_->get(current_game_id_, "mods_subpath", "") : std::string();

    for (const auto& idx : sel) {
        int r = idx.row();
        if (r < 0 || r >= mod_model_->mods().size()) continue;
        const auto& entry = mod_model_->mods()[r];
        if (entry.is_overwrite) continue;

        if (!mods_subpath.empty() && !current_game_dir_.empty()) {
            auto mod_folder = mods_dir_path() / entry.id.toStdString();
            if (!engine::remove_path(mod_folder)) {
                engine::Logger::instance().error("Failed to move mod folder to trash: " +
                    mod_folder.string());
            }
        }
        mod_model_->remove_mod(entry.id);
    }
}

void MainWindow::move_to_separator(const QString& mod_id, const QString& sep_id) {
    mod_model_->set_separator_id(mod_id, sep_id);

    // Move mod row to right after the separator row
    const auto& mods = mod_model_->mods();
    int sep_row = -1;
    for (int i = 0; i < mods.size(); ++i) {
        if (mods[i].is_separator && mods[i].id == sep_id) {
            sep_row = i;
            break;
        }
    }
    if (sep_row >= 0)
        mod_model_->move_mod(mod_id, sep_row + 1);
}

void MainWindow::send_to_highest_priority(const QString& id) {
    if (mod_model_->is_conflict_order_reversed()) {
        // Isaac: lowest priority number = highest priority = top of list
        mod_model_->move_mod(id, 0);
    } else {
        // Standard (MO2): highest priority number = highest priority = bottom of list
        int ow_row = mod_model_->overwrite_row();
        int target = ow_row >= 0 ? ow_row - 1 : mod_model_->mods().size() - 1;
        if (target < 0) target = 0;
        mod_model_->move_mod(id, target);
    }
}

void MainWindow::send_to_lowest_priority(const QString& id) {
    if (mod_model_->is_conflict_order_reversed()) {
        // Isaac: highest priority number = lowest priority = bottom of list
        int ow_row = mod_model_->overwrite_row();
        int target = ow_row >= 0 ? ow_row - 1 : mod_model_->mods().size() - 1;
        if (target < 0) target = 0;
        mod_model_->move_mod(id, target);
    } else {
        // Standard (MO2): lowest priority number = lowest priority = top of list
        mod_model_->move_mod(id, 0);
    }
}

void MainWindow::send_to_highest_in_separator(const QString& id) {
    const auto& mods = mod_model_->mods();
    int mod_row = mod_model_->priority_of(id);
    if (mod_row < 0) return;

    QString sep_id = mods[mod_row].separator_id;
    if (sep_id.isEmpty()) return;

    int sep_row = -1;
    for (int i = mod_row - 1; i >= 0; --i) {
        if (mods[i].is_separator && mods[i].id == sep_id) {
            sep_row = i;
            break;
        }
    }
    if (sep_row < 0) return;
    mod_model_->move_mod(id, sep_row + 1);
}

void MainWindow::send_to_lowest_in_separator(const QString& id) {
    const auto& mods = mod_model_->mods();
    int mod_row = mod_model_->priority_of(id);
    if (mod_row < 0) return;

    QString sep_id = mods[mod_row].separator_id;
    if (sep_id.isEmpty()) return;

    int ow_row = mod_model_->overwrite_row();
    int target = ow_row >= 0 ? ow_row : mods.size();

    for (int i = mod_row + 1; i < mods.size(); ++i) {
        if (mods[i].is_separator) {
            target = i;
            break;
        }
    }
    mod_model_->move_mod(id, target - 1);
}

void MainWindow::priority_move_selected(int step) {
    auto sel = mod_view_->selectionModel()->selectedRows();
    if (sel.isEmpty()) return;

    int r = sel.first().row();
    const auto& mods = mod_model_->mods();
    if (r < 0 || r >= mods.size()) return;
    const auto& e = mods[r];
    if (e.is_separator || e.is_overwrite || e.is_merged) return;

    int target = r + step;
    if (target < 0 || target >= mods.size()) return;
    if (mods[target].is_separator || mods[target].is_overwrite || mods[target].is_merged) return;

    mod_model_->move_mod(e.id, target);
}

void MainWindow::toggle_selected_mods(bool enabled) {
    auto sel = mod_view_->selectionModel()->selectedRows();
    for (const auto& idx : sel) {
        int r = idx.row();
        if (r < 0 || r >= mod_model_->mods().size()) continue;
        const auto& entry = mod_model_->mods()[r];
        if (entry.is_separator || entry.is_overwrite || entry.is_game_native) continue;

        // Check if state would actually change
        if (entry.enabled == enabled) continue;

        mod_model_->setData(mod_model_->index(r, ModListModel::Enabled),
            enabled ? Qt::Checked : Qt::Unchecked, Qt::CheckStateRole);
        sync_mod_enable_state(entry.id, enabled);
    }
}

void MainWindow::rename_selected_mod() {
    auto sel = mod_view_->selectionModel()->selectedRows();
    if (sel.isEmpty()) return;
    int row = sel.first().row();
    if (row < 0 || row >= mod_model_->mods().size()) return;
    const auto& entry = mod_model_->mods()[row];
    if (entry.is_separator || entry.is_overwrite) return;

    if (!knowledge_ || current_game_id_.empty() || current_game_dir_.empty()) return;
    auto mods_subpath = knowledge_->get(current_game_id_, "mods_subpath", "");
    if (mods_subpath.empty()) return;

    bool ok;
    auto new_name = QInputDialog::getText(this, tr("Rename Mod"),
        tr("New name:"), QLineEdit::Normal, entry.name, &ok);
    if (!ok || new_name.trimmed().isEmpty()) return;
    if (new_name.trimmed() == entry.name) return;

    auto old_folder = mods_dir_path() / entry.id.toStdString();
    auto new_id = entry.id;  // keep same folder name on disk
    // Actually: rename the folder on disk using the new display name
    // But the folder name is the id, not the display name...
    // For simplicity, just update the display name in the model.
    // The folder stays the same.

    // Record the new display name in the meta dir sidecar
    auto meta_dir = meta_dir_path();
    if (!meta_dir.empty()) {
        auto meta = engine::ModMeta::load(meta_dir, entry.id.toStdString());
        meta.set("General", "name", new_name.trimmed().toStdString());
        meta.save(meta_dir, entry.id.toStdString());
    }

    // Update model directly
    int mod_idx = row;
    auto& mods = const_cast<QVector<ModEntry>&>(mod_model_->mods());
    if (mod_idx < mods.size()) {
        mods[mod_idx].name = new_name.trimmed();
        emit mod_model_->dataChanged(mod_model_->index(mod_idx, ModListModel::Name),
                                      mod_model_->index(mod_idx, ModListModel::Name));
    }

    engine::Logger::instance().debug("Renamed mod: " + entry.name.toStdString() +
        " -> " + new_name.trimmed().toStdString());
}

SourceVisitInfo MainWindow::source_visit_info(const QString& source_type, const QString& source_id) const {
    if (source_type == "steam") {
        return {tr("Visit on Workshop"),
            QString("https://steamcommunity.com/sharedfiles/filedetails/?id=%1").arg(source_id)};
    }
    if (source_type == "nexus") {
        auto domain = QString::fromStdString(
            knowledge_ ? knowledge_->get(current_game_id_, "nexus_domain", "") : "");
        if (domain.isEmpty()) domain = source_id;
        return {tr("Visit on Nexus"),
            QString("https://www.nexusmods.com/%1/mods/%2").arg(domain, source_id)};
    }
    if (source_type == "loverslab") {
        return {tr("Visit on LoversLab"),
            QString("https://www.loverslab.com/files/file/%1/").arg(source_id)};
    }
    if (source_type == "moddb") {
        return {tr("Visit on ModDB"),
            QString("https://www.moddb.com/mods/%1").arg(source_id)};
    }
    auto label = source_type;
    if (!label.isEmpty()) {
        label[0] = label[0].toUpper();
    }
    return {tr("Visit on %1").arg(label), QString()};
}

QString MainWindow::create_separator_named(const QString& name, const QString& color) {
    if (!knowledge_ || current_game_id_.empty() || current_game_dir_.empty()) return {};

    auto mods_subpath = knowledge_->get(current_game_id_, "mods_subpath", "");
    auto separator_suffix = knowledge_->get(current_game_id_, "separator_suffix", "_separator");
    if (mods_subpath.empty()) return {};

    // Guard against duplicate names
    if (mod_model_->existing_separator_names().contains(name)) return {};

    auto folder_name = name.toStdString() + separator_suffix;
    auto sep_dir = mods_dir_path() / folder_name;

    // Create the separator folder
    std::error_code ec;
    std::filesystem::create_directories(sep_dir, ec);
    if (ec) return {};

    // Write separator.xml
    auto xml_path = sep_dir / "separator.xml";
    std::ofstream f(xml_path);
    if (!f) return {};
    f << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    f << "<separator>\n";
    f << "  <name>" << name.toStdString() << "</name>\n";
    f << "  <color>" << color.toStdString() << "</color>\n";
    f << "</separator>\n";
    f.close();

    // Add to model
    auto id = QString::fromStdString(folder_name);
    mod_model_->add_separator(id, name, color);
    engine::Logger::instance().debug("Separator created: " + name.toStdString());
    return id;
}

void MainWindow::create_separator() {
    if (!knowledge_ || current_game_id_.empty() || current_game_dir_.empty()) return;

    bool ok;
    auto name = QInputDialog::getText(this, tr("Create Separator"),
        tr("Separator name:"), QLineEdit::Normal, QString(), &ok);
    if (!ok || name.trimmed().isEmpty()) return;

    // Check for duplicate names
    if (mod_model_->existing_separator_names().contains(name.trimmed())) {
        QMessageBox::warning(this, tr("Separator"), tr("A separator with this name already exists."));
        return;
    }

    // Pick color
    QColor color = QColorDialog::getColor(QColor("#888888"), this, tr("Separator Color"));
    if (!color.isValid()) return;

    if (create_separator_named(name.trimmed(), color.name()).isEmpty()) {
        QMessageBox::warning(this, tr("Separator"), tr("Failed to create separator directory."));
    }
}

void MainWindow::create_empty_mod() {
    if (!knowledge_ || current_game_id_.empty() || current_game_dir_.empty()) return;

    bool ok;
    auto name = QInputDialog::getText(this, tr("Create Empty Mod"),
        tr("Mod name:"), QLineEdit::Normal, QString(), &ok);
    if (!ok || name.trimmed().isEmpty()) return;

    // Check for duplicate names
    QString trimmed = name.trimmed();
    for (const auto& m : mod_model_->mods()) {
        if (!m.is_separator && !m.is_overwrite && !m.is_merged &&
            (m.name.compare(trimmed, Qt::CaseInsensitive) == 0 ||
             m.id.compare(trimmed, Qt::CaseInsensitive) == 0)) {
            QMessageBox::warning(this, tr("Create Empty Mod"),
                tr("A mod with this name already exists."));
            return;
        }
    }

    // Sanitize the folder name (drop path separators and reserved characters)
    QString folder = trimmed;
    folder.replace(QRegularExpression(R"([/\\:*?"<>|])"), "_");

    auto mods_dir = mods_dir_path();
    std::error_code ec;
    std::filesystem::create_directories(mods_dir, ec);
    if (ec) {
        QMessageBox::warning(this, tr("Create Empty Mod"),
            tr("Failed to create mods directory."));
        return;
    }

    auto mod_dir = mods_dir / folder.toStdString();
    if (std::filesystem::exists(mod_dir, ec)) {
        QMessageBox::warning(this, tr("Create Empty Mod"),
            tr("A folder named %1 already exists in the mods directory.").arg(folder));
        return;
    }
    std::filesystem::create_directories(mod_dir, ec);
    if (ec) {
        QMessageBox::warning(this, tr("Create Empty Mod"),
            tr("Failed to create mod folder."));
        return;
    }

    // Write the game's metadata file into the mod folder so ModScanner picks
    // the mod up. MO2-style games get a meta.ini (same keys MO2 and
    // InstallStage write); XML games (Isaac) get their metadata.xml.
    auto metadata_file = knowledge_->get(current_game_id_, "metadata_file", "meta.ini");
    engine::ModMeta::write_game_metadata(mod_dir, metadata_file,
                                         trimmed.toStdString(), "1.0", "0");

    engine::Logger::instance().debug("Empty mod created: " + folder.toStdString());
    load_mods_from_game();
}

void MainWindow::import_archives(const QStringList& paths) {
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
}

namespace {

// Minimal RFC-4180 CSV parser (handles quoted fields, doubled quotes,
// commas/newlines inside quotes).
std::vector<QStringList> parse_csv(const QByteArray& data) {
    std::vector<QStringList> rows;
    const QString text = QString::fromUtf8(data);
    QStringList current;
    QString field;
    bool in_quotes = false;
    for (int i = 0; i < text.size(); ++i) {
        const QChar c = text[i];
        if (in_quotes) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') {
                    field += '"';
                    ++i;
                } else {
                    in_quotes = false;
                }
            } else {
                field += c;
            }
        } else if (c == '"') {
            in_quotes = true;
        } else if (c == ',') {
            current << field;
            field.clear();
        } else if (c == '\n') {
            current << field;
            rows.push_back(current);
            current.clear();
            field.clear();
        } else if (c != '\r') {
            field += c;
        }
    }
    if (!field.isEmpty() || !current.isEmpty()) {
        current << field;
        rows.push_back(current);
    }
    return rows;
}

QString csv_escape(const QString& field) {
    if (!field.contains(',') && !field.contains('"') && !field.contains('\n'))
        return field;
    QString quoted = field;
    quoted.replace("\"", "\"\"");
    return "\"" + quoted + "\"";
}

}  // namespace

void MainWindow::export_modlist() {
    if (current_game_id_.empty()) return;
    const QString path = QFileDialog::getSaveFileName(this,
        tr("Export Modlist"), QString(), tr("CSV files (*.csv);;All files (*)"));
    if (path.isEmpty()) return;

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, tr("Export Modlist"), tr("Failed to write file."));
        return;
    }

    auto write_row = [&](const QStringList& fields) {
        QStringList escaped;
        escaped.reserve(fields.size());
        for (const auto& field : fields) escaped << csv_escape(field);
        f.write(escaped.join(",").toUtf8());
        f.write("\n");
    };

    write_row({QStringLiteral("type"), QStringLiteral("priority"),
               QStringLiteral("name"), QStringLiteral("source_link"),
               QStringLiteral("color"), QStringLiteral("modid"),
               QStringLiteral("folder_name")});

    const auto& mods = mod_model_->mods();
    int exported = 0;
    for (int i = 0; i < mods.size(); ++i) {
        const auto& m = mods[i];
        if (m.is_overwrite || m.is_merged) continue;
        if (m.is_separator) {
            write_row({QStringLiteral("separator"), QString::number(i),
                       m.name, QString(), m.separator_color, QString(), m.id});
        } else {
            QString source;
            if (!m.source_type.isEmpty())
                source = source_visit_info(m.source_type, m.source_id).url;
            write_row({QStringLiteral("mod"), QString::number(i),
                       m.name, source, QString(), m.source_id, m.id});
        }
        ++exported;
    }
    f.close();
    engine::Logger::instance().info("Modlist exported: " +
        std::to_string(exported) + " entries");
}

void MainWindow::import_modlist() {
    if (current_game_id_.empty()) return;
    const QString path = QFileDialog::getOpenFileName(this,
        tr("Import Modlist"), QString(), tr("CSV files (*.csv);;All files (*)"));
    if (path.isEmpty()) return;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("Import Modlist"), tr("Failed to open file."));
        return;
    }
    auto rows = parse_csv(f.readAll());
    f.close();

    if (rows.empty()) {
        QMessageBox::warning(this, tr("Import Modlist"), tr("Invalid modlist file."));
        return;
    }

    // Skip the header row if present
    size_t start = 0;
    if (rows[0].value(0).trimmed() == QLatin1String("type")) start = 1;

    // Match by strict priority: modid > folder name > display name. Each
    // criterion gets its own full pass so an early name collision can't
    // shadow a later, stronger folder/modid match.
    auto find_row = [this](const QString& modid, const QString& folder_name,
                           const QString& name, bool want_separator) -> int {
        const auto& mods = mod_model_->mods();
        auto match_any = [&mods, want_separator](const QString& key,
                                                 QString ModEntry::*field) -> int {
            if (key.isEmpty()) return -1;
            for (int i = 0; i < mods.size(); ++i) {
                const auto& m = mods[i];
                if (m.is_overwrite || m.is_merged) continue;
                if (m.is_separator != want_separator) continue;
                if ((m.*field).compare(key, Qt::CaseInsensitive) == 0) return i;
            }
            return -1;
        };
        int idx = match_any(modid, &ModEntry::source_id);
        if (idx < 0) idx = match_any(folder_name, &ModEntry::id);
        if (idx < 0) idx = match_any(name, &ModEntry::name);
        return idx;
    };

    // Batch the reorder with disk syncs suppressed; persist once at the end.
    loading_ = true;
    int placed = 0;
    int created = 0;
    int missing = 0;
    int cursor = 0;
    for (size_t r = start; r < rows.size(); ++r) {
        const auto& row = rows[r];
        QString type = row.value(0).trimmed().toLower();
        QString name = row.value(2).trimmed();
        QString modid = row.value(5).trimmed();
        QString folder_name = row.value(6).trimmed();
        bool is_separator = (type == QLatin1String("separator"));

        int idx = is_separator
            ? find_row(QString(), folder_name, name, true)
            : find_row(modid, folder_name, name, false);
        if (idx < 0 && is_separator && !name.isEmpty()) {
            QString color = row.value(4).trimmed();
            if (create_separator_named(name,
                    color.isEmpty() ? QStringLiteral("#888888") : color).isEmpty()) {
                ++missing;
                continue;
            }
            ++created;
            idx = find_row(QString(), QString(), name, true);
        }
        if (idx < 0) {
            ++missing;
            continue;
        }

        const auto& mods = mod_model_->mods();
        if (idx != cursor) mod_model_->move_mod(mods[idx].id, cursor);
        ++cursor;
        ++placed;
    }
    loading_ = false;

    save_order();
    sync_priorities();
    sync_separator_ids();
    apply_mod_filter();

    const int total = static_cast<int>(rows.size() - start);
    engine::Logger::instance().info("Modlist import: " + std::to_string(placed) +
        " of " + std::to_string(total) + " placed, " + std::to_string(created) +
        " separators created, " + std::to_string(missing) + " missing");
    QMessageBox::information(this, tr("Import Modlist"),
        tr("Placed %1 of %2 entries in order. %3 separator(s) created. %4 not found.")
            .arg(placed).arg(total).arg(created).arg(missing));
}

void MainWindow::create_separator_at_row(int row) {
    if (!knowledge_ || current_game_id_.empty() || current_game_dir_.empty()) return;

    bool ok;
    auto name = QInputDialog::getText(this, tr("Create Separator"),
        tr("Separator name:"), QLineEdit::Normal, QString(), &ok);
    if (!ok || name.trimmed().isEmpty()) return;

    // Check for duplicate names
    if (mod_model_->existing_separator_names().contains(name.trimmed())) {
        QMessageBox::warning(this, tr("Separator"), tr("A separator with this name already exists."));
        return;
    }

    // Pick color
    QColor color = QColorDialog::getColor(QColor("#888888"), this, tr("Separator Color"));
    if (!color.isValid()) return;

    auto id = create_separator_named(name.trimmed(), color.name());
    if (id.isEmpty()) {
        QMessageBox::warning(this, tr("Separator"), tr("Failed to create separator directory."));
        return;
    }

    // Move the new separator to the target row (below the clicked row)
    int insert_row = row + 1;
    mod_model_->move_mod(id, insert_row);
    engine::Logger::instance().debug("Separator created at row " + std::to_string(insert_row) +
        ": " + name.toStdString());
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
    auto new_name = QInputDialog::getText(this, tr("Edit Separator"),
        tr("Separator name:"), QLineEdit::Normal, mod.name, &ok);
    if (!ok || new_name.trimmed().isEmpty()) return;

    // Check for duplicate names (excluding self)
    auto existing = mod_model_->existing_separator_names();
    existing.removeAll(mod.name);
    if (existing.contains(new_name.trimmed())) {
        QMessageBox::warning(this, tr("Separator"), tr("A separator with this name already exists."));
        return;
    }

    // Pick color
    QColor current_color(mod.separator_color.isEmpty() ? "#888888" : mod.separator_color);
    QColor color = QColorDialog::getColor(current_color, this, tr("Separator Color"));
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
            QMessageBox::warning(this, tr("Separator"), tr("Failed to rename separator folder."));
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

    auto reply = QMessageBox::question(this, tr("Delete Separator"),
        tr("Move separator \"%1\" to the trash bin?\n\nIt can be restored from the system trash.")
            .arg(mod.name),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    if (!knowledge_ || current_game_id_.empty() || current_game_dir_.empty()) return;

    auto mods_subpath = knowledge_->get(current_game_id_, "mods_subpath", "");
    if (!mods_subpath.empty() && !current_game_dir_.empty()) {
        auto sep_folder = mods_dir_path() / mod.id.toStdString();
        if (!engine::remove_path(sep_folder)) {
            engine::Logger::instance().error("Failed to move separator folder to trash: " + sep_folder.string());
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
            if (!mods[i].is_overwrite && !mods[i].is_merged)
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
        // Overwrite always first, MERGED always second (only for games that use it)
        reordered.insert(0, ModEntry());
        reordered[0].is_overwrite = true;
        reordered[0].id = kOverwriteModId;
        reordered[0].name = kOverwriteModName;
        reordered[0].enabled = true;
        reordered[0].priority = 0;
        if (mod_model_->uses_merged()) {
            reordered.insert(1, ModEntry());
            reordered[1].is_merged = true;
            reordered[1].id = kMergedModId;
            reordered[1].name = kMergedModName;
            reordered[1].enabled = true;
            reordered[1].priority = 1;
        }
        mod_model_->reset_with_order(reordered);
        mod_model_->renumber_priorities();
        engine::Logger::instance().debug("Migrated from mod_order (" + std::to_string(order.size()) + " entries)");
    } else {
        // New path: sort by priority from meta.ini
        QVector<ModEntry> sorted = mods;
        // Mods without a persisted priority (-1) sort to the bottom of the user
        // band, never the top - otherwise a freshly installed mod would win the
        // list (top = priority 0).
        auto key = [](const ModEntry& e) { return e.priority < 0 ? 1000000 : e.priority; };
        std::stable_sort(sorted.begin(), sorted.end(), [&key](const ModEntry& a, const ModEntry& b) {
            if (a.is_overwrite) return false;
            if (b.is_overwrite) return true;
            if (a.is_merged) return false;
            if (b.is_merged) return true;
            return key(a) < key(b);
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

    sync_separator_ids();
}

void MainWindow::save_executables() {
    if (current_instance_root_.empty()) return;

    auto toml_path = current_instance_root_ / "instance.toml";

    std::ifstream in(toml_path);
    std::string existing;
    if (in) {
        existing.assign((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    }
    in.close();

    std::istringstream stream(existing);
    std::string line;
    std::string cleaned;
    while (std::getline(stream, line)) {
        auto key_pos = line.find("executables");
        if (key_pos != std::string::npos) continue;
        cleaned += line + "\n";
    }

    // Collect JSON objects from the combo
    auto entries = right_panel_->exec_controls()->executable_entries();
    QStringList json_entries;
    for (const auto& e : entries) {
        auto raw = QString::fromUtf8(QJsonDocument(e.toJson()).toJson(QJsonDocument::Compact));
        json_entries.append(raw);
    }

    cleaned += "executables = [\n";
    for (int i = 0; i < json_entries.size(); ++i) {
        if (i > 0) cleaned += ",\n";
        cleaned += "    " + json_entries[i].toStdString();
    }
    cleaned += "\n]\n";

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

    auto start = content.find("executables = [");
    if (start == std::string::npos) return;
    start += std::string("executables = [").size();
    auto end = content.find(']', start);
    if (end == std::string::npos) return;

    auto section = content.substr(start, end - start);

    // Parse entries: handle both JSON objects {..} and plain "strings"
    // Walk character by character tracking brace depth
    size_t i = 0;
    while (i < section.size()) {
        // Skip whitespace and commas
        while (i < section.size() && (section[i] == ' ' || section[i] == '\t'
               || section[i] == '\n' || section[i] == '\r' || section[i] == ','))
            ++i;
        if (i >= section.size()) break;

        if (section[i] == '{') {
            // JSON object - find matching close brace
            int depth = 0;
            auto obj_start = i;
            while (i < section.size()) {
                if (section[i] == '{') ++depth;
                else if (section[i] == '}') {
                    --depth;
                    if (depth == 0) {
                        ++i;  // include the closing brace
                        break;
                    }
                } else if (section[i] == '"') {
                    // Skip past quoted string
                    ++i;
                    while (i < section.size() && !(section[i] == '"' && section[i-1] != '\\'))
                        ++i;
                }
                ++i;
            }
            saved_executables_.push_back(section.substr(obj_start, i - obj_start));
        } else if (section[i] == '"') {
            // Plain string - find closing quote
            auto str_start = i;
            ++i;
            while (i < section.size() && !(section[i] == '"' && section[i-1] != '\\'))
                ++i;
            if (i < section.size()) ++i;  // include closing quote
            auto raw = section.substr(str_start, i - str_start);
            // Wrap legacy plain strings in JSON
            saved_executables_.push_back("{\"path\":" + raw + "}");
        } else {
            // Skip unexpected characters
            ++i;
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

    // Prefer saved executables list (persists user additions across restarts)
    QStringList exec_list;
    if (!saved_executables_.empty()) {
        for (const auto& s : saved_executables_)
            exec_list.append(QString::fromStdString(s));
    } else {
        // First launch - seed from game plugin's known executables
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
    // Restore the last selected executable for this instance. On a fresh
    // instance the selection is empty - just populate the list and let the
    // user pick.
    right_panel_->exec_controls()->set_executables(exec_list, pending_exec_selection_, current_game_dir_, icon_cache);

    // Persist immediately on first run so future launches use the saved list
    if (saved_executables_.empty())
        save_executables();
}

void MainWindow::launch_game() {
    auto exec_rel = right_panel_->exec_controls()->current_executable();
    if (exec_rel.isEmpty() || exec_rel == kAddNewEntryText) {
        QMessageBox::warning(this, tr("Launch"), tr("No executable selected."));
        return;
    }
    if (current_game_dir_.empty()) {
        QMessageBox::warning(this, tr("Launch"), tr("Game directory not set."));
        return;
    }

    auto exec_path = current_game_dir_ / exec_rel.toStdString();
    if (!std::filesystem::exists(exec_path)) {
        engine::Logger::instance().warn(
            "executable does not exist: " + exec_path.string());
        QMessageBox::warning(this, tr("Launch"),
            tr("The selected executable no longer exists:\n%1\n\n"
               "The entry has been removed.")
                .arg(QString::fromStdString(exec_path.string())));
        auto* bar = right_panel_->exec_controls();
        auto entries = bar->executable_entries();
        bar->clear_executables();
        for (const auto& e : entries) {
            if (e.path == exec_rel) continue;
            bar->add_entry(e);
        }
        save_executables();
        return;
    }
    launch_with_executable(QString::fromStdString(exec_path.string()));
}

static void gmm_debug(const char* fmt, ...) {
    static bool enabled = (std::getenv("GMM_DEBUG") != nullptr);
    if (!enabled) return;
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[GMM] ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}

void MainWindow::launch_with_executable(const QString& full_path) {
    auto& trace = engine::TraceRecorder::instance();
    trace.begin_flow("launch");

    auto exec_path = std::filesystem::path(full_path.toStdString());
    if (!std::filesystem::exists(exec_path)) {
        QMessageBox::warning(this, tr("Launch"),
            tr("Executable not found:\n%1").arg(full_path));
        trace.end_flow("launch", false, "Executable not found");
        return;
    }

    // Show the lock overlay before launching - the game must not outrun it
    auto binary_name = QString::fromStdString(exec_path.filename().string());
    show_game_lock_overlay(binary_name, 0);

    // Ensure disk order matches UI before launching
    trace.begin_stage("launch", "Sync disk order");
    sync_priorities();
    trace.end_stage("launch", true, "Disk order matches UI");

    // Read steam_appid from game plugin hooks - 0 if not registered
    uint32_t steam_appid = 0;
    if (knowledge_) {
        auto id_str = knowledge_->get(current_game_id_, "steam_appid", "");
        if (!id_str.empty()) {
            try { steam_appid = std::stoul(id_str); } catch (...) {}
        }
    }

    trace.begin_stage("launch", "Prepare launch environment");
    engine::LaunchParams lparams;
    lparams.executable = exec_path;
    lparams.game_dir = current_game_dir_;
    lparams.overwrite_dir = current_instance_root_ / "overwrite";
    lparams.steam_appid = steam_appid;
    lparams.is_windows_exe = (exec_path.extension().string() == ".exe" ||
                              exec_path.extension().string() == ".EXE");
    if (!staging_dir_.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(staging_dir_, ec);
        if (!ec) {
            lparams.extra_lowerdirs.push_back(staging_dir_);
        } else {
            engine::Logger::instance().error(
                "Failed to create staging dir: " + ec.message());
        }
    }
    trace.end_stage("launch", true, "Overlay/staging paths ready");

    trace.begin_stage("launch", "Launch executable");
    auto lresult = engine::launch_game(lparams);

    if (lresult.pid <= 0) {
        trace.end_stage("launch", false, "launch_game returned no PID");
        hide_game_lock_overlay();
        QMessageBox::warning(this, tr("Launch"), tr("Failed to launch game."));
        trace.end_flow("launch", false, "Failed to launch game");
        return;
    }
    trace.end_stage("launch", true,
        lresult.overlay_launched
            ? "Launched via OverlayFS / LD_PRELOAD overlay"
            : "Launched via Native/Proton runtime");

    overlay_launched_ = lresult.overlay_launched;
    running_process_pid_ = lresult.pid;
    cgroup_path_ = lresult.cgroup_path;
    launch_time_ = std::filesystem::file_time_type::clock::now();

    // Update overlay with actual PID now that we have it
    game_lock_label_->setText(tr("The game is running: %1 (%2)")
        .arg(binary_name)
        .arg(lresult.pid));

    // Monitor process - stays Running until the watchdog sees the game exit
    trace.begin_stage("launch", "Monitor process");

    if (!process_watch_timer_) {
        process_watch_timer_ = new QTimer(this);
        process_watch_timer_->setInterval(2000);
        connect(process_watch_timer_, &QTimer::timeout, this, [this]() {
            check_running_process();
        });
    }
    process_watch_timer_->start();
}

void MainWindow::check_running_process() {
    auto& trace = engine::TraceRecorder::instance();
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
        trace.end_stage("launch", true, "Game exited");
        trace.end_flow("launch", true, "Game session finished");
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
    // ---- cgroup v2 path (primary) ----
    if (!cgroup_path_.empty()) {
        if ((process_tree_checkbox_ && process_tree_checkbox_->isChecked())
            && !engine::cgroup_is_empty({cgroup_path_}))
            refresh_process_tree();

        if (engine::cgroup_is_empty({cgroup_path_})) {
            engine::Logger::instance().debug(
                "Watchdog: cgroup empty, game fully exited");
            reap_supervisor(static_cast<pid_t>(running_process_pid_));
            flush_pending_changes();
            hide_game_lock_overlay();
            trace.end_stage("launch", true, "Game exited");
            trace.end_flow("launch", true, "Game session finished");
            running_process_pid_ = -1;
            cgroup_path_.clear();
            if (process_watch_timer_) process_watch_timer_->stop();
            if (!staging_dir_.empty()) {
                std::error_code ec;
                std::filesystem::remove_all(staging_dir_, ec);
                staging_dir_.clear();
            }
            auto t = launch_time_;
            QTimer::singleShot(3000, this, [this, t]() { do_capture_overwrite(t); });
        }
        return;
    }

    // ---- fallback: subreaper + PGID / PPID walk ----
    // Try to reap the root child on every tick.  is_process_group_alive()
    // / kill(-pgid,0) considers zombie processes "alive" (they still have
    // a task_struct entry), which would gate cleanup and cause a permanent
    // zombie.  Early-reaping prevents that.
    int reap_status;
    pid_t early_reaped = waitpid(static_cast<pid_t>(running_process_pid_),
                                 &reap_status, WNOHANG);
    if (early_reaped > 0) {
        engine::Logger::instance().debug("Watchdog: early-reaped child PID " +
            std::to_string(early_reaped));
    }

    // Linux: track the entire process group (PGID), not a single PID.
    int64_t pgid = running_process_pid_;

    if (engine::is_process_group_alive(pgid)) {
        if (process_tree_checkbox_ && process_tree_checkbox_->isChecked())
            refresh_process_tree();
        return;
    }

    // PGID scan found nothing - try PPID descendant walk.
    // This finds processes that created new sessions via setsid().
    {
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
    }

    engine::Logger::instance().debug("Watchdog: process group " +
        std::to_string(pgid) + " fully exited, scheduling capture in 3s");

    // Safety reap - covers the case where the root PID was already collected
    // above but a second waitpid on the same PID is harmless (returns ECHILD).
    pid_t reap_result = waitpid(static_cast<pid_t>(pgid), &reap_status, WNOHANG);
    if (reap_result == pgid) {
        engine::Logger::instance().debug("Watchdog: reaped child PID " + std::to_string(pgid));
    } else if (reap_result < 0 && errno != ECHILD) {
        engine::Logger::instance().error("Watchdog: waitpid(" + std::to_string(pgid) +
            ") failed: " + std::strerror(errno));
    }

    flush_pending_changes();
    hide_game_lock_overlay();
    trace.end_stage("launch", true, "Game exited");
    trace.end_flow("launch", true, "Game session finished");
    running_process_pid_ = -1;
    cgroup_path_.clear();
    if (process_watch_timer_) process_watch_timer_->stop();
    if (!staging_dir_.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(staging_dir_, ec);
        staging_dir_.clear();
    }
    auto t = launch_time_;
    QTimer::singleShot(3000, this, [this, t]() { do_capture_overwrite(t); });
#endif
}

void MainWindow::apply_mod_filter() {
    if (!mod_model_ || !mod_view_) return;

    // Start from a clean fold state
    mod_model_->apply_fold_state();

    const QString text = filter_bar_->filter_text().trimmed().toLower();
    const QString group = filter_bar_->current_group();
    const auto& mods = mod_model_->mods();

    // First pass: compute visibility for each mod row
    QVector<bool> visible(mods.size(), false);
    for (int row = 0; row < mods.size(); ++row) {
        const auto& m = mods[row];

        // Separators: determined in second pass
        if (m.is_separator) continue;

        // Text filter: match against name or id
        bool text_match = text.isEmpty()
            || m.name.toLower().contains(text)
            || m.id.toLower().contains(text);

        // Group filter
        bool group_match = true;
        if (group == "Enabled")
            group_match = m.enabled;
        else if (group == "Disabled")
            group_match = !m.enabled;
        else if (group == "Conflicts")
            group_match = (m.conflict_wins > 0 || m.conflict_losses > 0);
        else if (group == "Separators")
            group_match = false;  // regular mods hidden when viewing separators only

        visible[row] = text_match && group_match;

        // If the separator above is folded, hide this row (fold overrides search)
        if (visible[row] && !m.separator_id.isEmpty() && group == "All") {
            for (int i = row - 1; i >= 0; --i) {
                if (mods[i].is_separator && mods[i].id == m.separator_id) {
                    if (mods[i].folded) {
                        visible[row] = false;
                    }
                    break;
                }
            }
        }
    }

    // Second pass: separators are shown only if at least one child mod is visible
    for (int row = 0; row < mods.size(); ++row) {
        if (!mods[row].is_separator) continue;

        if (group == "Separators") {
            visible[row] = true;
        } else if (text.isEmpty() && (group == "All" || group == "Enabled" || group == "Disabled" || group == "Conflicts")) {
            visible[row] = true;
        } else {
            // Scan children for any visible mod
            visible[row] = false;
            for (int j = row + 1; j < mods.size() && !mods[j].is_separator; ++j) {
                if (visible[j]) {
                    visible[row] = true;
                    break;
                }
            }
        }

        mod_view_->setRowHidden(row, QModelIndex(), !visible[row]);
    }

    // Apply visibility to all mod rows
    for (int row = 0; row < mods.size(); ++row) {
        if (mods[row].is_separator) continue;
        mod_view_->setRowHidden(row, QModelIndex(), !visible[row]);
    }
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

    if (running_process_pid_ <= 0) return;

    struct ProcInfo { pid_t pid; pid_t ppid; std::string name; char state; };
    std::vector<ProcInfo> procs;

    // When cgroup is available, restrict the scan to its members only.
    // Otherwise scan all of /proc and use PPID walk from the root PID.
    std::unordered_set<pid_t> cgroup_set;
    bool use_cgroup = !cgroup_path_.empty();
    if (use_cgroup) {
        for (int64_t p : engine::cgroup_members({cgroup_path_}))
            cgroup_set.insert(static_cast<pid_t>(p));
    }

    DIR* dir = opendir("/proc");
    if (!dir) return;
    struct dirent* entry;
    while ((entry = readdir(dir))) {
        if (entry->d_type != DT_DIR) continue;
        pid_t pid = atol(entry->d_name);
        if (pid <= 0) continue;

        if (use_cgroup && !cgroup_set.count(pid))
            continue;

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

        procs.push_back({pid, ppid, comm, state});
    }
    closedir(dir);

    if (procs.empty()) return;

    // When using cgroup, the PID list is already complete (no PPID walk needed).
    // Otherwise, run PPID descendant walk from the root PID.
    if (!use_cgroup) {
        pid_t root_pid = static_cast<pid_t>(running_process_pid_);
        if (root_pid <= 0) return;

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

        if (procs.empty()) return;
    }

    std::unordered_map<pid_t, QTreeWidgetItem*> items;

    for (const auto& pr : procs) {
        auto* item = new QTreeWidgetItem;
        item->setText(0, QString::fromStdString(pr.name));
        item->setText(1, QString::number(pr.pid));
        item->setText(2, QString(QChar(pr.state)));
        items[pr.pid] = item;
    }

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
    auto entry = right_panel_->exec_controls()->current_entry();
    if (entry.path.isEmpty()) {
        QMessageBox::warning(this, tr("Shortcut"), tr("No executable selected."));
        return;
    }
    if (current_game_dir_.empty()) {
        QMessageBox::warning(this, tr("Shortcut"), tr("Game directory not set."));
        return;
    }

    auto exec_path = current_game_dir_ / entry.path.toStdString();
    if (!std::filesystem::exists(exec_path)) {
        QMessageBox::warning(this, tr("Shortcut"),
            tr("Executable not found:\n%1").arg(QString::fromStdString(exec_path.string())));
        return;
    }

    auto exec_path_qstr = QString::fromStdString(exec_path.string());
    add_toolbar_shortcut_from_path(exec_path_qstr, entry.icon_path);
}

void MainWindow::add_toolbar_shortcut_from_path(const QString& full_path,
                                                  const QString& icon_path) {
    if (toolbar_shortcut_paths_.contains(full_path)) return;
    if (!QFileInfo::exists(full_path)) return;

    QIcon icon;
    if (!icon_path.isEmpty()) {
        QPixmap pix(icon_path);
        if (!pix.isNull())
            icon = QIcon(pix);
    }
    if (icon.isNull())
        icon = extractExeIconShortcut(full_path);

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
    auto entry = right_panel_->exec_controls()->current_entry();
    if (entry.path.isEmpty()) {
        QMessageBox::warning(this, tr("Shortcut"), tr("No executable selected."));
        return;
    }
    if (current_game_dir_.empty()) {
        QMessageBox::warning(this, tr("Shortcut"), tr("Game directory not set."));
        return;
    }

    auto exec_path = current_game_dir_ / entry.path.toStdString();
    if (!std::filesystem::exists(exec_path)) {
        QMessageBox::warning(this, tr("Shortcut"),
            tr("Executable not found:\n%1").arg(QString::fromStdString(exec_path.string())));
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
        QMessageBox::warning(this, tr("Shortcut"), tr("Could not determine desktop directory."));
        return;
    }

    // Build the .desktop file content (XDG standard)
    auto exec_qstr = QString::fromStdString(exec_path.string());
    // Escape % sign for .desktop files
    exec_qstr.replace("%", "%%");

    auto desktop_file = desktop + "/" + game_name.replace(" ", "_") + ".desktop";
    QFile f(desktop_file);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, tr("Shortcut"),
            tr("Failed to create desktop file:\n%1").arg(desktop_file));
        return;
    }

    QTextStream out(&f);
    out << "[Desktop Entry]\n";
    out << "Type=Application\n";
    out << "Name=" << game_name << "\n";
    out << "Exec=" << exec_qstr << "\n";
    out << "Path=" << QString::fromStdString(current_game_dir_.string()) << "\n";
    {
        auto icon_for_desktop = entry.icon_path.isEmpty()
            ? QString::fromStdString(exec_path.string())
            : entry.icon_path;
        out << "Icon=" << icon_for_desktop << "\n";
    }
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
    QMessageBox::information(this, tr("Shortcut"),
        tr("Desktop shortcut created:\n%1").arg(desktop_file));
}

void MainWindow::on_add_entry_requested() {
    // Collect mod list for the "Output to mod" dropdown
    QVector<QPair<QString, QString>> mod_list;
    if (mod_model_) {
        for (const auto& m : mod_model_->mods()) {
            if (!m.is_separator && !m.is_overwrite && !m.is_merged) {
                mod_list.append({m.id, m.name});
            }
        }
    }

    auto icon_cache = current_instance_root_.empty()
        ? std::filesystem::path{}
        : current_instance_root_ / "cache" / "thumbnails";

    auto existing = right_panel_->exec_controls()->executable_entries();

    // Prune dead entries (binary no longer exists) before showing the dialog.
    // Deliberately not at startup: a temporarily unavailable game dir must not
    // wipe the list. Pruning only persists if the user accepts the dialog.
    if (!current_game_dir_.empty()) {
        QVector<ExecEntry> pruned;
        pruned.reserve(existing.size());
        for (const auto& e : existing) {
            auto resolved = current_game_dir_ / e.path.toStdString();
            if (!e.path.trimmed().isEmpty() && !std::filesystem::exists(resolved)) {
                engine::Logger::instance().warn(
                    "removing dead executable entry '" +
                    exec_entry_display_name(e).toStdString() +
                    "' (does not exist: " + resolved.string() + ")");
                continue;
            }
            pruned.append(e);
        }
        existing = pruned;
    }

    ExecEntryDialog dlg(current_game_dir_, mod_list, existing, icon_cache, this);
    if (dlg.exec() != QDialog::Accepted) return;

    auto all_entries = dlg.entries();

    // Replace the entire combo content, then re-apply the selection the user
    // had before opening the editor. Editing must not move the combo to the
    // first or last entry - the selection follows the user until app close.
    auto* bar = right_panel_->exec_controls();
    auto prev_selection = bar->current_executable();
    bar->clear_executables();
    for (const auto& e : all_entries) {
        bar->add_entry(e);
    }
    if (!prev_selection.isEmpty())
        bar->select_executable(prev_selection);
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

// --- Game-lock overlay ---

void MainWindow::create_game_lock_overlay() {
    game_lock_overlay_ = new QWidget(this);
    game_lock_overlay_->setObjectName("gameLockOverlay");

    auto* layout = new QVBoxLayout(game_lock_overlay_);
    layout->setAlignment(Qt::AlignCenter);
    layout->setContentsMargins(40, 40, 40, 40);

    layout->addStretch(2);

    game_lock_label_ = new QLabel(tr("The game is running"), game_lock_overlay_);
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

    auto* tree_label = new QLabel(tr("Show process tree"), game_lock_overlay_);
    tree_label->setObjectName("processTreeLabel");
    tree_row->addWidget(tree_label);

    layout->addLayout(tree_row);

    process_tree_ = new QTreeWidget(game_lock_overlay_);
    process_tree_->setHeaderLabels({tr("Name"), "PID", "S"});
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
    auto* copy_tree_btn = new QPushButton(tr("Copy"), game_lock_overlay_);
    copy_tree_btn->setFixedSize(52, 22);
    copy_tree_btn->setToolTip(tr("Copy process tree structure to clipboard"));
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

    unlock_button_ = new QPushButton(tr("Unlock"), game_lock_overlay_);
    unlock_button_->setObjectName("unlockBtn");
    QObject::connect(unlock_button_, &QPushButton::clicked, this, [this]() {
        hide_game_lock_overlay();
    });
    btn_layout->addWidget(unlock_button_);

    kill_button_ = new QPushButton(tr("Kill"), game_lock_overlay_);
    kill_button_->setObjectName("killBtn");
    QObject::connect(kill_button_, &QPushButton::clicked, this, [this]() {
        if (running_process_pid_ <= 0) {
            flush_pending_changes();
            hide_game_lock_overlay();
            return;
        }

        // ---- cgroup v2 kill (primary) ----
        if (!cgroup_path_.empty()) {
            engine::cgroup_kill({cgroup_path_});
            engine::Logger::instance().debug(
                "Kill: cgroup.kill written for " + cgroup_path_);
            reap_supervisor(static_cast<pid_t>(running_process_pid_));
            running_process_pid_ = -1;
            cgroup_path_.clear();
            if (process_watch_timer_) process_watch_timer_->stop();
            flush_pending_changes();
            hide_game_lock_overlay();
            return;
        }

        // ---- fallback: process group kill ----
        pid_t pgid = static_cast<pid_t>(running_process_pid_);
        if (pgid <= 0) {
            flush_pending_changes();
            hide_game_lock_overlay();
            return;
        }

        auto reap_or_schedule = [this](pid_t pid) {
            int status;
            pid_t ret = waitpid(pid, &status, WNOHANG);
            if (ret == pid) {
                engine::Logger::instance().debug("Kill: reaped child " + std::to_string(pid));
                return true;
            }
            if (ret == 0) {
                QTimer::singleShot(3000, this, [this, pid]() {
                    int s;
                    waitpid(pid, &s, WNOHANG);
                });
                return false;
            }
            return true;
        };

        int ret = kill(-pgid, SIGTERM);
        if (ret != 0) {
            int err = errno;
            if (err == ESRCH) {
                engine::Logger::instance().debug("Kill: process group " + std::to_string(pgid) + " already empty");
                reap_or_schedule(pgid);
                running_process_pid_ = -1;
                if (process_watch_timer_) process_watch_timer_->stop();
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
                QMessageBox::warning(this, tr("Kill Failed"),
                    tr("Failed to terminate process group %1: %2")
                        .arg(static_cast<long long>(pgid))
                        .arg(std::strerror(err)));
                return;
            }
        }
        engine::Logger::instance().debug("Kill: terminated process group " + std::to_string(pgid));
        reap_or_schedule(pgid);
        running_process_pid_ = -1;
        if (process_watch_timer_) process_watch_timer_->stop();
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
    if (pid > 0) {
    game_lock_label_->setText(tr("The game is running: %1 (%2)")
            .arg(binary_name)
            .arg(pid));
    } else {
        game_lock_label_->setText(tr("Launching %1 …").arg(binary_name));
    }

    game_lock_overlay_->setGeometry(rect());
    game_lock_overlay_->raise();
    game_lock_overlay_->show();
    if (pid > 0 && process_tree_checkbox_ && process_tree_checkbox_->isChecked())
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

    engine::Logger::instance().debug("Flushing " + std::to_string(pending_changes_.size()) +
        " queued mod changes");

    auto mods_subpath = knowledge_->get(current_game_id_, "mods_subpath", "");
    if (mods_subpath.empty()) {
        engine::Logger::instance().warn("Cannot flush changes: mods_subpath is empty");
        pending_changes_.clear();
        return;
    }

    // Apply toggles (latest state per mod wins - already deduplicated by sync_mod_enable_state)
    for (const auto& pt : pending_changes_) {
        auto mod_folder = resolve_mod_folder(pt.mod_id.toStdString(), mods_subpath);
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
    engine::Logger::instance().debug("Queued mod changes flushed");
}

void MainWindow::update_queue_label() {
    if (!pending_queue_label_) return;
    if (pending_changes_.empty()) {
        pending_queue_label_->hide();
        return;
    }
    pending_queue_label_->setText(tr("Changes queued: %1 (apply on game exit)")
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
                engine::Logger::instance().debug("Debug mode enabled (Konami code entered)");
                status_bar_->set_status(tr("Debug mode enabled"));
            } else {
                std::filesystem::remove(flag, ec);
                engine::Logger::instance().debug("Debug mode disabled");
                status_bar_->set_status(tr("Debug mode disabled"));
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
    header_states["icon_size"] = icon_size_;
    if (right_panel_ && right_panel_->exec_controls()) {
        auto cur_exec = right_panel_->exec_controls()->current_executable();
        if (!cur_exec.isEmpty())
            header_states["selected_exec"] = cur_exec;
    }
    // Match read_ba() below: every block in this file is stored base64.
    // (Historical note: this used to be written raw while read_ba() decoded
    // fromBase64, which silently corrupted the whole extra block - selected
    // executable, icon size, and right-panel header states never restored.)
    QByteArray extra = QJsonDocument(header_states).toJson(QJsonDocument::Compact).toBase64();
    write_ba(extra);
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
    auto obj = read_app_state_extra();
    if (!obj.isEmpty() && right_panel_) {
        // Restore process tree visibility (prefixed with _ to avoid tab-name collision)
        if (obj.contains("_process_tree_visible"))
            show_process_tree_ = obj["_process_tree_visible"].toBool();
        // Restore toolbar icon size (Small/Medium/Large)
        if (obj.contains("icon_size")) {
            icon_size_ = obj["icon_size"].toInt(24);
            menu_bar_->set_icon_size(icon_size_);
            if (toolbar_area_)
                toolbar_area_->setIconSize(QSize(icon_size_, icon_size_));
            if (toolbar_)
                toolbar_->set_icon_size(icon_size_);
        }
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
                // Re-apply desired stretch modes so restoreState
                // doesn't permanently override them from old sessions
                if (key == "Data") {
                    auto* h = table->horizontalHeader();
                    h->setStretchLastSection(false);
                    h->setSectionResizeMode(0, QHeaderView::Stretch);
                    h->setSectionResizeMode(1, QHeaderView::Interactive);
                    h->setSectionResizeMode(2, QHeaderView::Interactive);
                } else if (key == "Downloads") {
                    auto* h = table->horizontalHeader();
                    h->setStretchLastSection(false);
                    h->setSectionResizeMode(0, QHeaderView::Stretch);
                    h->setSectionResizeMode(1, QHeaderView::Interactive);
                    h->setSectionResizeMode(2, QHeaderView::Interactive);
                    h->setSectionResizeMode(3, QHeaderView::Interactive);
                }
            }
        }
    }
}

QJsonObject MainWindow::read_app_state_extra() const {
    QJsonObject empty;
    auto path = app_state_path();
    if (!std::filesystem::exists(path)) return empty;
    std::ifstream in(path, std::ios::binary);
    if (!in) return empty;

    auto read_ba = [&]() -> QByteArray {
        uint32_t len = 0;
        if (!in.read(reinterpret_cast<char*>(&len), sizeof(len)) || len == 0) return {};
        std::vector<char> buf(len);
        if (!in.read(buf.data(), len)) return {};
        return QByteArray::fromBase64(QByteArray(buf.data(), len));
    };

    // Skip the five fixed blocks (geometry, window state, splitters, header)
    for (int i = 0; i < 5; ++i) read_ba();

    uint32_t extra_len = 0;
    if (!in.read(reinterpret_cast<char*>(&extra_len), sizeof(extra_len)) || extra_len == 0) return empty;
    std::vector<char> buf(extra_len);
    if (!in.read(buf.data(), extra_len)) return empty;
    QByteArray raw(buf.data(), extra_len);
    auto doc = QJsonDocument::fromJson(QByteArray::fromBase64(raw));
    if (!doc.isObject())
        doc = QJsonDocument::fromJson(raw);  // legacy: extra written raw
    return doc.object();
}

void MainWindow::restore_exec_selection() {
    auto obj = read_app_state_extra();
    if (obj.contains("selected_exec"))
        pending_exec_selection_ = obj["selected_exec"].toString();
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

    // Redact the signed download key in the log - it is a bearer token. The
    // rest of the URL (expires, user_id, param names) stays visible so a
    // session can verify whether the browser delivered the query string.
    std::string log_url = link.full_url;
    {
        auto kp = log_url.find("key=");
        if (kp != std::string::npos) {
            auto ke = log_url.find_first_of("&", kp);
            log_url = log_url.substr(0, kp + 4) +
                      (ke != std::string::npos ? log_url.substr(ke) : "");
        }
    }

    engine::Logger::instance().debug(
        "NXM download: domain=" + link.nexus_domain +
        " mod=" + std::to_string(link.mod_id) +
        " file=" + std::to_string(link.file_id) +
        " key=" + (link.key.empty() ? "absent" :
                   "present(" + std::to_string(link.key.size()) + "B)") +
        " expires=" + (link.expire > 0 ? std::to_string(link.expire) : "none") +
        " url=" + log_url);

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
        QMessageBox::warning(this, tr("NXM Download"),
            tr("Unknown Nexus Mods domain: %1\nNo game plugin supports this domain.")
                .arg(QString::fromStdString(link.nexus_domain)));
        return;
    }

    // Is this game managed by us?
    bool is_managed = managed_games_ && managed_games_->is_managed(matched_game_id);

    if (!is_managed) {
        QMessageBox::information(this, tr("NXM Download"),
            tr("This mod is for %1, but GameModManager is not managing this game.\n\n"
               "Open the game's instance first to register it, then try the link again.")
                .arg(QString::fromStdString(plugin_loader_->display_name_for(matched_game_id))));
        return;
    }

    // Game is managed - but is the active instance the right one?
    if (matched_game_id != current_game_id_) {
        QMessageBox::information(this, tr("NXM Download"),
            tr("This mod is for %1, but the active instance is %2.\n"
               "Switch to the correct instance first.")
                .arg(QString::fromStdString(plugin_loader_->display_name_for(matched_game_id)))
                .arg(QString::fromStdString(current_game_name_)));
        return;
    }

    // Route to the active instance - start download via pipeline worker
    engine::Logger::instance().debug(
        "Starting download: " + current_game_name_ +
        " (mod_id=" + std::to_string(link.mod_id) +
        ", file_id=" + std::to_string(link.file_id) + ")");

    // Show in DownloadsTab immediately. The entry key is "<mod_id>-<file_id>"
    // so Main and Optional files of the same mod page stay separate entries.
    const auto mod_id = std::to_string(link.mod_id);
    const auto file_id = std::to_string(link.file_id);
    const auto key = mod_id + "-" + file_id;

    auto* dt = right_panel_->downloads_tab();
    if (dt) {
        dt->add_download(
            key,
            tr("Mod #%1 - file %2").arg(QString::fromStdString(mod_id))
                                    .arg(link.file_id).toStdString(),
            "Nexus Mods", {}, link.nexus_domain, link.file_id, mod_id);
    }

    // Surface the download: bring the window to front and switch to the
    // Downloads tab so the user sees the new entry start.
    if (isMinimized()) {
        showNormal();
    }
    raise();
    activateWindow();
    right_panel_->show_downloads_tab();

    // Keep the NXM link so a paused download can be resumed later.
    nxm_links_[key] = link;

    // Build paths for the pipeline context
    auto mods_dir = mods_dir_path();
    auto meta_dir = current_instance_root_.empty()
        ? "" : (current_instance_root_ / "meta").string();

    // Invoke the pipeline worker asynchronously (download only - install is a
    // separate user-triggered step)
    QMetaObject::invokeMethod(pipeline_thread_->worker(), [this, key, link, mods_dir, meta_dir]() {
        pipeline_thread_->worker()->download_mod(
            key, link, current_game_id_, mods_dir.string(), meta_dir);
    }, Qt::QueuedConnection);

    engine::Logger::instance().debug("Download queued for mod " + mod_id + " file " + file_id);
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
    msg.setWindowTitle(tr("Nexus Mods Downloads"));
    msg.setText(tr("Do you want to enable Nexus Mods downloads for <b>%1</b>?")
        .arg(QString::fromStdString(current_game_name_)));
    msg.setTextFormat(Qt::RichText);
    msg.setIcon(QMessageBox::Question);
    msg.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    msg.setDefaultButton(QMessageBox::Yes);
    auto reply = msg.exec();

    if (reply == QMessageBox::Yes) {
        managed_games_->add_source(current_game_id_,
            engine::GameSource{"nexus", "nexusmods.com", nexus_domain});

        engine::Logger::instance().debug(
            "Registered " + current_game_id_ + " for Nexus Mods downloads (nexusmods.com)");
    }
}

void MainWindow::ensure_nxm_handler_default() {
    if (nxm_handler_check_done_) return;
    nxm_handler_check_done_ = true;

#ifdef GMM_PLATFORM_LINUX
    // Respect a permanent "don't ask again" choice
    QSettings settings("GameModManager", "GameModManager");
    if (settings.value("nxm/handler_check").toString() == "dont_ask") return;

    // Self-heal: if we're still the default handler, nothing to do
    if (engine::LinuxPlatform::is_nxm_handler_registered()) {
        engine::Logger::instance().debug("nxm:// handler check: GameModManager is the default");
        return;
    }

    auto app_path = std::filesystem::path(
        QCoreApplication::applicationFilePath().toStdString());

    engine::Logger::instance().info(
        "nxm:// handler check: GameModManager is NOT the default — prompting");
    QMessageBox msg(this);
    msg.setWindowTitle(tr("NXM Protocol Handler"));
    msg.setText(tr("GameModManager is no longer the default app for "
                   "<b>nxm://</b> download links from Nexus Mods.\n\n"
                   "Make it the default again?"));
    msg.setTextFormat(Qt::RichText);
    msg.setIcon(QMessageBox::Question);
    auto* yes = msg.addButton(tr("Yes"), QMessageBox::YesRole);
    auto* dont_show = msg.addButton(tr("Don't show"), QMessageBox::ActionRole);
    auto* no = msg.addButton(tr("No"), QMessageBox::NoRole);
    msg.setDefaultButton(yes);
    msg.exec();

    if (msg.clickedButton() == yes) {
        (void)engine::LinuxPlatform::register_nxm_handler(app_path);
        (void)engine::LinuxPlatform::register_gmm_handler(app_path);
        engine::Logger::instance().info("nxm:// handler registered: GameModManager");
    } else if (msg.clickedButton() == dont_show) {
        settings.setValue("nxm/handler_check", "dont_ask");
        engine::Logger::instance().debug("nxm:// handler check suppressed (don't show again)");
    } else {
        engine::Logger::instance().debug("nxm:// handler check declined; will ask next launch");
    }
#else
    (void)0;
#endif
}

void MainWindow::show_settings_dialog() {
    auto& auth = engine::NexusAuth::instance();
    bool has_key = auth.has_api_key();

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Settings"));
    dlg.setMinimumWidth(480);

    auto* layout = new QVBoxLayout(&dlg);

    // -- Language section ----------------------------------------
    auto* lang_group = new QGroupBox(tr("Language"));
    auto* lang_layout = new QVBoxLayout(lang_group);

    QSettings settings("GameModManager", "GameModManager");
    const QString current_lang = settings.value("language", "en_US").toString();

    auto* lang_combo = new QComboBox;
    QDir i18n_dir(":/i18n");
    for (const auto& info : i18n_dir.entryInfoList({"*.qm"}, QDir::Files | QDir::NoDotAndDotDot)) {
        const QString tag = info.completeBaseName();
        const QLocale loc(tag);
        const QString display = loc.language() != QLocale::C
            ? loc.nativeLanguageName() + " (" + tag + ")"
            : tag;
        lang_combo->addItem(display, tag);
    }
    int lang_idx = lang_combo->findData(current_lang);
    lang_combo->setCurrentIndex(lang_idx >= 0 ? lang_idx : 0);

    auto* lang_hint = new QLabel(
        tr("Restart the application for the language change to take effect."));
    lang_hint->setWordWrap(true);

    lang_layout->addWidget(lang_combo);
    lang_layout->addWidget(lang_hint);
    layout->addWidget(lang_group);

    // -- API Key section ----------------------------------------
    auto* api_group = new QGroupBox(tr("Nexus Mods API Key"));
    auto* api_layout = new QVBoxLayout(api_group);

    auto* key_edit = new QLineEdit;
    key_edit->setEchoMode(QLineEdit::Password);
    key_edit->setPlaceholderText(tr("Enter your Nexus Mods API key..."));
    if (has_key)
        key_edit->setText(QString::fromStdString(auth.get_api_key()));

    auto* key_row = new QHBoxLayout;
    key_row->addWidget(key_edit, 1);

    auto* save_btn = new QPushButton(has_key ? tr("Update") : tr("Save"));
    auto* clear_btn = new QPushButton(tr("Clear"));
    clear_btn->setEnabled(has_key);
    key_row->addWidget(save_btn);
    key_row->addWidget(clear_btn);

    api_layout->addLayout(key_row);
    api_layout->addWidget(new QLabel(
        tr("Get your key at "
           "<a href='https://www.nexusmods.com/users/myaccount?tab=api'>"
           "nexusmods.com/users/myaccount?tab=api</a>")));
    layout->addWidget(api_group);

    // -- Rate-limit section --------------------------------------
    auto* rl_group = new QGroupBox(tr("API Rate Limit"));
    auto* rl_layout = new QVBoxLayout(rl_group);

    auto info = auth.get_rate_limit();
    auto* rl_label = new QLabel;
    if (info.daily_limit > 0 || info.hourly_limit > 0) {
        QString text;
        auto budget_line = [&](const QString& name, int remaining, int limit, int64_t reset) {
            QString line = tr("%1: <b>%2</b> / %3")
                .arg(name).arg(remaining).arg(limit);
            if (reset > 0) {
                QDateTime dt = QDateTime::fromSecsSinceEpoch(reset);
                line += tr(" &nbsp;(resets %1)")
                    .arg(dt.toLocalTime().toString(Qt::TextDate));
            }
            return line;
        };
        if (info.hourly_limit > 0)
            text += budget_line(tr("Hourly"), info.hourly_remaining, info.hourly_limit, info.hourly_reset);
        if (info.daily_limit > 0) {
            if (!text.isEmpty()) text += "<br>";
            text += budget_line(tr("Daily"), info.daily_remaining, info.daily_limit, info.daily_reset);
        }
        if (info.last_updated > 0) {
            QDateTime lu = QDateTime::fromSecsSinceEpoch(info.last_updated);
            text += "<br>" + tr("Last request: %1").arg(lu.toLocalTime().toString(Qt::TextDate));
        }
        rl_label->setText(text);
    } else {
        rl_label->setText(tr("No API requests made yet in this session."));
    }
    rl_layout->addWidget(rl_label);
    layout->addWidget(rl_group);

    // -- Dialog buttons ------------------------------------------
    layout->addSpacing(8);
    auto* btn_box = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(btn_box, &QDialogButtonBox::rejected, &dlg, &QDialog::close);
    layout->addWidget(btn_box);

    // -- Button actions ------------------------------------------
    connect(lang_combo, &QComboBox::currentIndexChanged, this, [lang_combo](int index) {
        QSettings("GameModManager", "GameModManager")
            .setValue("language", lang_combo->itemData(index).toString());
    });

    connect(save_btn, &QPushButton::clicked, [&]() {
        QString key = key_edit->text().trimmed();
        if (key.isEmpty()) {
            QMessageBox::warning(&dlg, tr("API Key"),
                tr("Enter your Nexus Mods API key or click Clear to remove it."));
            return;
        }
        auth.set_api_key(key.toStdString());
        clear_btn->setEnabled(true);
        save_btn->setText(tr("Update"));
        engine::Logger::instance().info("Nexus API key saved");
        QMessageBox::information(&dlg, tr("API Key"), tr("API key saved successfully."));
    });

    connect(clear_btn, &QPushButton::clicked, [&]() {
        auto reply = QMessageBox::question(&dlg, tr("Clear API Key"),
            tr("Remove the stored Nexus Mods API key?"),
            QMessageBox::Yes | QMessageBox::No);
        if (reply == QMessageBox::Yes) {
            auth.clear_api_key();
            key_edit->clear();
            clear_btn->setEnabled(false);
            save_btn->setText(tr("Save"));
            engine::Logger::instance().info("Nexus API key cleared");
        }
    });

    dlg.exec();
}

void MainWindow::show_instance_statistics() {
    if (current_instance_root_.empty()) {
        QMessageBox::information(this, tr("Instance Statistics"),
                                 tr("No instance is currently loaded."));
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

void MainWindow::show_pipeline_window() {
    if (!pipeline_window_) {
        pipeline_window_ = new PipelineWindow(this);
    }
    pipeline_window_->refresh();
    pipeline_window_->show();
    pipeline_window_->raise();
    pipeline_window_->activateWindow();
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
        sel_dlg.setWindowTitle(tr("Create New Instance"));
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
            QMessageBox::warning(this, tr("Error"),
                tr("Failed to create instance for %1").arg(QString::fromStdString(chosen.display_name)));
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

    switch_to_instance(QString::fromStdString(selected));
}

bool MainWindow::switch_to_instance(const QString& name) {
    auto instances_dir = engine::default_instances_dir();
    auto selected = name.toStdString();
    if (selected.empty()) return false;

    // Don't reload if already on this instance
    if (!current_instance_root_.empty() && instances_dir / selected == current_instance_root_)
        return true;

    auto inst = engine::Instance::installed(selected, instances_dir);
    if (!inst.read_toml()) {
        QMessageBox::warning(this, tr("Error"),
            tr("Failed to read instance.toml for %1").arg(name));
        return false;
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
    return true;
}

void MainWindow::refresh_recent_instances() {
    if (!menu_bar_) return;
    auto instances_dir = engine::default_instances_dir();
    std::vector<std::string> names;
    std::error_code ec;
    if (std::filesystem::is_directory(instances_dir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(instances_dir, ec)) {
            if (!entry.is_directory()) continue;
            if (!std::filesystem::exists(entry.path() / "instance.toml")) continue;
            names.push_back(entry.path().filename().string());
        }
    }
    std::sort(names.begin(), names.end());
    menu_bar_->set_recent_instances(names);
}

}  // namespace ui
