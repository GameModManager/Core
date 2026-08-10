#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/wait.h>
#include "ui/main_window/main_window.h"
#include "ui/widgets/mod_list_model.h"
#include "ui/widgets/mod_table_view.h"
#include "ui/widgets/main_toolbar.h"
#include "ui/widgets/profile_bar.h"
#include "ui/widgets/mod_filter_bar.h"
#include "ui/widgets/column_toggle_header.h"
#include "ui/widgets/list_dialog.h"
#include "ui/widgets/right_panel.h"
#include "ui/widgets/exec_controls_bar.h"
#include "ui/widgets/console_panel.h"
#include "ui/widgets/gmm_status_bar.h"
#include "ui/widgets/menu_bar.h"
#include "ui/widgets/instance_statistics_dialog.h"
#include "ui/widgets/instance_switcher_dialog.h"
#include "ui/pipeline_worker.h"
#include "ui/main_window/loot_sort_worker.h"
#include "ui/main_window/conflict_scan_worker.h"
#include "ui/main_window/mod_scan_worker.h"
#include "ui/main_window/plugin_db_load_worker.h"
#include "ui/main_window/saves_scan_worker.h"
#include "ui/main_window/deploy_worker.h"
#include "ui/panels/tab_panels.h"
#include "ui/smooth_scroll.h"
#include "ui/settings/settings_dialog.h"
#include "ui/settings/settings.h"
#include "ui/proton/proton_panel.h"
#include "engine/launcher.h"
#include "engine/debug_env.h"
#include "engine/events/event_bus.h"
#include "engine/fs_utils.h"
#include "engine/theme/theme_manager.h"
#include "engine/theme/icon_manager.h"
#include "engine/log/logger.h"
#include "engine/detect/mod_scanner.h"
#include "engine/meta/categories.h"
#include "engine/detect/game_detector.h"
#include "engine/index/conflict_engine.h"
#include "engine/registry/game_features/game_feature_registry.h"
#include "engine/registry/game_knowledge.h"
#include "engine/instance/instance.h"
#include "engine/instance/instance_utils.h"
#include "engine/plugins/plugin_database.h"
#include "engine/nxm/nxm_router.h"
#include "engine/nxm/managed_games.h"
#include "engine/nxm/nxm_ipc.h"
#include "engine/pipeline/sync_stage.h"
#include "engine/overwrite/overwrite_utils.h"
#include "ui/overwrite/move_to_mod_dialog.h"
#include "ui/overwrite/overwrite_info_dialog.h"
#include "ui/overwrite/query_overwrite_dialog.h"
#include "ui/overwrite/sync_overwrite_dialog.h"
#include "ui/install/install_name_dialog.h"
#include "ui/install/install_progress_dialog.h"
#include "ui/fomod/fomod_wizard_dialog.h"
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
#include "engine/source/loverslab_provider.h"
#include "engine/nexus_auth.h"
#include "engine/loverslab_auth.h"
#include "engine/source/steam_workshop_provider.h"
#include "engine/pipeline/extract_stage.h"
#include "engine/pipeline/fomod_stage.h"
#include "engine/pipeline/install_stage.h"
#include "engine/pipeline/plugin_claim_stage.h"
#include "engine/trace/trace_recorder.h"
#include "engine/deploy/strategy.h"
#include "runtime/runtime.h"
#include "runtime/wine_runtime.h"
#include "ui/preview/preview_window.h"
#include "ui/modinfo/mod_info_dialog.h"
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
#include <QAbstractButton>
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
#include <QStyleFactory>
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
#include <set>
#include <sstream>
#include <thread>
#include <unordered_set>

namespace ui {

namespace {

// Reap the subreaper supervisor forked by engine::launcher_game(). It never
// execs (stays "[gamemodmanager]"), so if the watchdog stops without
// waitpid()ing it, it remains a zombie forever. A cgroup-empty result means
// the game and its descendants are gone, so the supervisor exits as soon as
// its reap loop hits ECHILD; poll briefly so a stray reparented daemon can't
// hang the UI thread. Returns the supervisor's exit code (WEXITSTATUS), or -1
// when it could not be reaped.
int reap_supervisor(pid_t pid) {
    if (pid <= 0) return -1;
    using namespace std::chrono;
    for (int attempt = 0; attempt < 20; ++attempt) {
        int status;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        if (r < 0 && errno == ECHILD) return -1;
        std::this_thread::sleep_for(milliseconds(100));
    }
    engine::Logger::instance().warn("Watchdog: supervisor " + std::to_string(pid) +
        " not reaped after 2s (stray child?)");
    return -1;
}

// Stable per-column name for persisting mod-list visibility (enum names, not
// indices, so the stored state survives a column reorder). Keep in sync with
// ModListModel::Column order.
QString mod_column_name(int column) {
    switch (column) {
        case ModListModel::Name:         return "Name";
        case ModListModel::Conflicts:    return "Conflicts";
        case ModListModel::Flags:        return "Flags";
        case ModListModel::Category:     return "Category";
        case ModListModel::Source:       return "Source";
        case ModListModel::SourceId:     return "Source ID";
        case ModListModel::Version:      return "Version";
        case ModListModel::Installation: return "Installation";
        case ModListModel::Changed:      return "Changed";
        case ModListModel::Priority:     return "Priority";
    }
    return {};
}

}  // anonymous namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent) {
    setWindowTitle(tr("GameModManager"));
    resize(1200, 800);

    // Conflict recompute infra (P8.1, THREADING.md §3.6): debounce + worker
    // thread so toggling/reordering a mod never blocks the UI on a full scan.
    conflict_debounce_timer_ = new QTimer(this);
    conflict_debounce_timer_->setSingleShot(true);
    conflict_debounce_timer_->setInterval(150);
    connect(conflict_debounce_timer_, &QTimer::timeout,
            this, &MainWindow::start_conflict_scan);

    // Plugin-discovery + order-persist debounce (P8.6, MO2 parity): a mod move
    // or toggle fires mod_list_changed repeatedly; the Plugins tab re-discovers
    // plugin files only when their availability actually changed (install /
    // remove / rename / toggle), and instance.toml is rewritten once at gesture
    // end instead of once per step.
    plugin_refresh_debounce_timer_ = new QTimer(this);
    plugin_refresh_debounce_timer_->setSingleShot(true);
    plugin_refresh_debounce_timer_->setInterval(250);
    connect(plugin_refresh_debounce_timer_, &QTimer::timeout,
            this, &MainWindow::refresh_plugins_tab);

    save_order_timer_ = new QTimer(this);
    save_order_timer_->setSingleShot(true);
    save_order_timer_->setInterval(300);
    connect(save_order_timer_, &QTimer::timeout, this, &MainWindow::save_order);

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
        int idx = toolbar_shortcut_paths_.indexOf(path);
        if (idx >= 0) {
            toolbar_shortcut_paths_.removeAt(idx);
            toolbar_shortcut_icons_.removeAt(idx);
        }
        save_order();
    });

    // --- Proton button: body opens the Proton options panel, arrow the menu ---
    {
        QIcon proton_icon = engine::IconManager::instance().resolve_icon(
            "proton", QStyle::SP_ComputerIcon);
        toolbar_->add_proton_button(proton_icon);

        auto* proton_menu = new QMenu(this);
        proton_menu->addAction(tr("Run winecfg"), this, [this]() {
            run_prefix_tool({"winecfg"});
        });
        proton_menu->addAction(tr("Run winetricks"), this, [this]() {
            run_prefix_tool({});
        });
        proton_menu->addAction(tr("Run an .exe in this prefix..."), this,
                               &MainWindow::run_exe_in_prefix);

        proton_menu->addSeparator();

        proton_menu->addAction(tr("Open Wine Registry"), this, [this]() {
            run_prefix_tool({"regedit"});
        });
        proton_menu->addAction(tr("Install a DLL..."), this, [this]() {
            // winetricks `dlls` lands straight on the "Install a Windows DLL
            // or component" picker.
            run_prefix_tool({"dlls"});
        });

        proton_menu->addSeparator();

        proton_menu->addAction(tr("Install recommended packages"), this, [this]() {
            show_proton_panel();
        });

        toolbar_->set_proton_menu(proton_menu);
        connect(toolbar_, &MainToolbar::proton_clicked, this, &MainWindow::show_proton_panel);
    }

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
        // P1.3 event bus: mirror MO2 onProfileChanged.
        engine::EventBus::instance().dispatch(
            engine::events::kProfileChanged,
            engine::json_obj({{"profile", current_profile_name_}}));
    });

    connect(profile_bar_, &ProfileBar::open_folder_requested, this,
            &MainWindow::open_folder);
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
            auto* ct = right_panel_->conflicts_tab();
            if (ct) ct->clear_content();
            return;
        }
        const auto& mods = mod_model_->mods();
        if (current.row() >= 0 && current.row() < mods.size() &&
            !mods[current.row()].is_separator && !mods[current.row()].is_overwrite) {
            auto& selected = mods[current.row()];

            // Push conflict data to the ConflictsTab
            auto* ct = right_panel_->conflicts_tab();
            if (ct) {
                ct->show_conflicts(selected.id, mods,
                                   last_conflict_registry_,
                                   mod_model_->conflict_pairs(),
                                   mod_model_->is_conflict_order_reversed());
            }
        } else {
            auto* ct = right_panel_->conflicts_tab();
            if (ct) ct->clear_content();
        }
    });

    // Mod selection -> highlight the mod's plugins in the plugins list
    // (union across multi-selection, MO2's highlightPlugins parity) and feed
    // the conflict-highlight union (row tint + scrollbar marks).
    connect(mod_view_->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &MainWindow::on_mod_selection_changed);

    // Alternating row colors follow the system palette's AlternateBase (no
    // custom bake: a setPalette() here would freeze the view and its children
    // against system Light<->Dark switches).

    // Sync checkbox toggles to filesystem (disable.it)
    connect(mod_model_, &QAbstractItemModel::dataChanged,
            this, [this](const QModelIndex& topLeft, const QModelIndex& bottomRight, const QVector<int>& roles) {
        (void)bottomRight;
        if (roles.contains(Qt::CheckStateRole) && topLeft.column() == ModListModel::Name) {
            // Id must be the on-disk folder name (mod.id): the Name column's
            // EditRole returns the display name (mod.name), which for XML-meta
            // games (Isaac) strips the _<workshopId> suffix. Resolving the
            // display name against the mods dir yields a nonexistent path and
            // the disable.it write fails silently (bug: no disable marker on
            // toggle, ever, for Isaac).
            int row = topLeft.row();
            if (row < 0 || row >= static_cast<int>(mod_model_->mods().size())) return;
            auto id = mod_model_->mods()[row].id;
            bool enabled = mod_model_->data(topLeft, Qt::CheckStateRole).toInt() == Qt::Checked;
            sync_mod_enable_state(id, enabled);

            // Enabling/disabling a mod changes which plugins the virtual Data
            // serves (plugins_db skips disabled mods) - re-discover on toggle.
            request_plugin_refresh();

            // Update status bar mod count
            int count = 0;
            for (const auto& m : mod_model_->mods()) {
                if (!m.is_separator && !m.is_overwrite && m.enabled) ++count;
            }
            status_bar_->set_counter_value(count);
        }
    });

    // Sync priority rewrites to metadata files after reorder (only the moved
    // rows, via the model's dirty set) and recompute conflict stats (debounced,
    // MO2 modPrioritiesChanged -> clearCaches). The Plugins tab is NOT refreshed
    // here: it only reacts to plugin-file availability changes (install/remove/
    // rename/toggle), debounced via request_plugin_refresh() from those sites —
    // reorders and folds never re-discover plugins (MO2 parity).
    connect(mod_model_, &ModListModel::mod_list_changed, this, [this]() {
        sync_priorities();
        recompute_conflicts();
    });

    // Save order (debounced) and re-apply per-row fold/group state on every
    // model change.
    connect(mod_model_, &ModListModel::mod_list_changed, this, [this]() {
        if (loading_) return;
        request_save_order();
        sync_separator_ids();
        apply_mod_filter();
    });

    // Inline rename (MO2 renameMod): the handler renames the folder on disk
    // and updates the row in place; on failure it reverts the editor.
    connect(mod_model_, &ModListModel::rename_requested,
            this, [this](int row, const QString& name) { apply_rename(row, name); });

    // Fold/unfold on the Fold column ONLY (the dedicated arrow cell, left of
    // Name). Clicks anywhere else on the separator row - including the whole
    // Name cell - must not fold. A separator with no content to hide shows an
    // empty Fold cell and its click is a dead no-op too. With nesting enabled
    // a mod with children folds the same way (hides its subtree).
    connect(mod_view_, &QTreeView::clicked, this, [this](const QModelIndex& idx) {
        if (!idx.isValid() || idx.column() != ModListModel::Fold) return;
        int row = idx.row();
        if (row < 0 || row >= mod_model_->mods().size()) return;
        if (!mod_model_->has_content(row)) return;

        const bool folded = mod_model_->mods()[row].folded;
        mod_model_->set_folded(row, !folded);
    });

    // Double-clicking the Overwrite row opens the shared info dialog (MO2's
    // default-action behavior for the Overwrite entry). Mod rows map the
    // clicked column to a Mod Info tab (MO2's modlistview.cpp double-click):
    // Version → Nexus, Flags → Conflicts, anything else → last-used tab.
    connect(mod_view_, &QTreeView::doubleClicked, this, [this](const QModelIndex& idx) {
        if (!idx.isValid()) return;
        int row = idx.row();
        if (row < 0 || row >= mod_model_->mods().size()) return;
        const auto& entry = mod_model_->mods()[row];
        if (entry.is_overwrite) {
            show_overwrite_info_dialog();
            return;
        }
        if (entry.is_separator || entry.is_game_native) return;

        int tab = -1;
        switch (idx.column()) {
        case ModListModel::Conflicts:  tab = static_cast<int>(ui::ModInfoTabId::Conflicts); break;
        case ModListModel::Flags:      tab = static_cast<int>(ui::ModInfoTabId::Conflicts); break;
        case ModListModel::Category:   tab = static_cast<int>(ui::ModInfoTabId::Categories); break;
        case ModListModel::Source:
        case ModListModel::SourceId:
        case ModListModel::Version:    tab = static_cast<int>(ui::ModInfoTabId::Source); break;
        default: break;  // last-used tab
        }
        on_data_mod_info(entry.id, tab);
    });

    // Drag-and-drop archives onto the mod list to install manually
    connect(mod_view_, &ModTableView::files_dropped, this, [this](const QStringList& paths) {
        import_archives(paths);
    });

    // Drag-and-drop files/folders out of the Overwrite info dialog onto a mod
    // row moves them into that mod (MO2's drop-to-mod).
    connect(mod_view_, &ModTableView::overwrite_files_dropped, this,
            [this](const QStringList& paths, int mod_row) {
                move_dropped_overwrite_files(paths, mod_row);
            });

    auto* mod_header = new ColumnToggleHeaderView(Qt::Horizontal, mod_view_);
    mod_header->set_column_labels({"", "Name", "Conflicts", "Flags", "Category",
                                   "Source", "Source ID", "Version", "Installation",
                                   "Changed", "Priority"});
    mod_header->set_section_tooltips({
        tr("Fold or unfold this separator (hides or shows its contents)"),
        tr("Name of the mod"),
        tr("Win/loss state of file conflicts with other mods"),
        tr("Badges: hidden files, FOMOD saved, root override, invalid data"),
        tr("Primary category of the mod"),
        tr("Site the mod was downloaded from"),
        tr("Mod/file ID on the source site"),
        tr("Version of the mod (if available)"),
        tr("When the mod folder was created (install/replace time)"),
        tr("Last time the mod folder was modified"),
        tr("Install priority: the higher, the more it overwrites"),
    });
    mod_view_->setHeader(mod_header);
    mod_header_ = mod_header;

    mod_header->setStretchLastSection(false);
    mod_header->setSectionsMovable(true);
    // The Fold arrow column is pinned to the left edge: fixed 24px cell,
    // always visible, and re-snapped to visual index 0 if the user tries to
    // drag another column past it (the arrow must stay aligned to the edge).
    mod_header->setSectionResizeMode(ModListModel::Fold, QHeaderView::Fixed);
    mod_header->resizeSection(ModListModel::Fold, 24);
    mod_header->setSectionResizeMode(ModListModel::Name, QHeaderView::Stretch);
    for (int c = ModListModel::Conflicts; c < ModListModel::ColumnCount; ++c)
        mod_header->setSectionResizeMode(c, QHeaderView::Interactive);
    mod_header->resizeSection(ModListModel::Conflicts, 80);
    mod_header->resizeSection(ModListModel::Flags, 80);
    mod_header->resizeSection(ModListModel::Category, 120);
    mod_header->resizeSection(ModListModel::Source, 40);
    mod_header->resizeSection(ModListModel::SourceId, 70);
    mod_header->resizeSection(ModListModel::Version, 80);
    mod_header->resizeSection(ModListModel::Installation, 90);
    mod_header->resizeSection(ModListModel::Changed, 90);
    mod_header->resizeSection(ModListModel::Priority, 60);

    // The Name and Fold columns can never be hidden (the context menu shows
    // them checked + disabled). Persist user visibility toggles per instance;
    // the restore happens on scan finish when the instance name is known.
    mod_header->set_locked_section(ModListModel::Name);
    mod_header->set_locked_section(ModListModel::Fold);
    connect(mod_header, &ColumnToggleHeaderView::section_toggled, this,
            [this](int logical, bool hidden) {
                if (logical == ModListModel::Name || logical == ModListModel::Fold ||
                    current_instance_root_.empty())
                    return;
                const auto key = QString::fromStdString(current_instance_root_.filename().string());
                const auto stored =
                    Settings::instance().modlist_hidden_columns(key);
                auto hidden_set = QSet<QString>(stored.cbegin(), stored.cend());
                const QString name = mod_column_name(logical);
                if (hidden) hidden_set.insert(name);
                else hidden_set.remove(name);
                Settings::instance().set_modlist_hidden_columns(key, hidden_set.values());
            });

    // Non-negotiable: the Fold arrow column stays at the left edge. Other
    // columns stay draggable, but any drag that displaces Fold from visual
    // index 0 is reverted. The recursive sectionMoved (from moveSection) sees
    // Fold already at 0 and returns, so this cannot loop.
    connect(mod_header, &QHeaderView::sectionMoved, this,
            [mod_header](int, int, int) {
                const int foldLogical = ModListModel::Fold;
                if (mod_header->visualIndex(foldLogical) == 0) return;
                mod_header->moveSection(foldLogical, 0);
            });

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
    // Nexus downloads queue one-at-a-time per Settings (free Regular/Supporter
    // accounts are throttled; see Settings::nexus_queue_downloads). Pushed at
    // startup and re-pushed after the settings dialog closes.
    pipeline_thread_->worker()->set_nexus_queue_downloads(
        Settings::instance().nexus_queue_downloads());

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

    // Download metadata resolved by the provider right before the bytes flow:
    // replace the "Mod #<id> - file <id>" / "LoversLab file <id>" placeholder
    // with the real name immediately, instead of only when the download ends.
    // Prefer the source-resolved mod/file name; fall back to the raw archive
    // name (extension stripped) so the row is descriptive either way.
    connect(pipeline_thread_->worker(), &PipelineWorker::download_meta,
            this, [this](const std::string& id, const std::string& archive_name,
                         const std::string& display_name) {
        auto* dt = right_panel_->downloads_tab();
        if (!dt) return;
        std::string name = display_name;
        if (name.empty() && !archive_name.empty())
            name = std::filesystem::path(archive_name).stem().string();
        if (!name.empty()) dt->rename_download(id, name);
    });

    // Install finished (user-triggered via the Downloads context menu or
    // double-click): add just the newly installed row to the mod list instead
    // of reloading the whole mods dir, and mark the entry Installed. The UI
    // lock put up at install_requested is released on every terminal signal
    // (success, failure, cancel). A failed install leaves the download row in
    // its download state - only a failed download marks it Failed.
    connect(pipeline_thread_->worker(), &PipelineWorker::install_complete,
            this, [this](const std::string& mod_id, bool success,
                         const std::string&, const std::string& installed_folder) {
        hide_install_progress();
        set_ui_enabled(true);
        auto* dt = right_panel_->downloads_tab();
        if (dt) {
            if (success) {
                if (!current_game_id_.empty() && !installed_folder.empty()) {
                    engine::Logger::instance().debug(
                        "Install finished for " + mod_id + ", adding " +
                        installed_folder);
                    add_installed_mod(installed_folder);
                    // P1.3 event bus: mirror MO2 onModInstalled. The bus
                    // handler runs synchronously here (install is UI-thread);
                    // a subscribed plugin must not block.
                    engine::EventBus::instance().dispatch(
                        engine::events::kModInstalled,
                        engine::json_obj({
                            {"mod", installed_folder},
                            {"name", installed_folder},
                        }));
                }
                dt->mark_installed(mod_id);
            }
            // Install failure is NOT a download failure: the row keeps its
            // download state (Complete, retryable Install button). The error
            // was already logged to the console by the pipeline worker /
            // extract stage - don't flip the download to Failed here.
        }
        // Persist download state
        save_download_manifest();
    });

    // Install canceled by the user (FOMOD wizard or overwrite dialog): NOT a
    // failure - leave the download in whatever state it had (no Failed mark).
    // Release the UI lock the same way as install_complete.
    connect(pipeline_thread_->worker(), &PipelineWorker::install_canceled,
            this, [this](const std::string&) {
        hide_install_progress();
        set_ui_enabled(true);
        save_download_manifest();
    });

    // Forward install-stage progress (extract/copy) to the install progress
    // popup. Emitted on the worker thread; this connection auto-queues it.
    connect(pipeline_thread_->worker(), &PipelineWorker::install_progress,
            this, &MainWindow::update_install_progress);

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
    engine::SourceRegistry::instance().register_provider(
        std::make_unique<engine::LoversLabProvider>());

    auto home = std::getenv("HOME");
    std::string ws_db = home
        ? (std::string(home) + "/.local/share/GameModManager/workshop_cache.db")
        : "workshop_cache.db";
    engine::SourceRegistry::instance().register_provider(
        std::make_unique<engine::SteamWorkshopProvider>(
            ws_db, Settings::instance().workshop_rate_limit_per_hour()));

    connect(right_panel_->exec_controls(), &ExecControlsBar::run_clicked, this, &MainWindow::launch_game);

    // LOOT sort shortcut from the Plugins tab filter bar
    connect(right_panel_, &RightPanel::sort_requested, this, &MainWindow::run_loot_sort);

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
    if (Settings::instance().smooth_scrolling())
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
    hide_install_progress();
    toolbar_->clear_exec_buttons();
    toolbar_shortcut_paths_.clear();
    toolbar_shortcut_icons_.clear();
    right_panel_->exec_controls()->clear_executables();
    nxm_links_.clear();  // NXM links are instance-scoped

    current_game_id_ = game_id;
    current_game_name_ = game_display_name;
    current_profile_name_ = profile_name;
    current_game_dir_ = game_dir;
    current_instance_root_ = instance_root;
    if (!instance_root.empty()) {
        current_instance_ = engine::Instance::from_root(instance_root);
        current_instance_.read_toml();
        conflict_cache_path_ = current_instance_.path_for(engine::InstanceKind::Cache) / "conflict_cache.json";
    }

    // The Overwrite folder may carry CI-duplicate directories (Meshes/ +
    // meshes/) left by an earlier session on the case-sensitive Linux fs: the
    // game's raw writes split one logical dir across casings. Fold them back
    // once per instance load (deferred so the switch is never blocked - it is
    // idempotent, a clean Overwrite costs one listing).
    if (!instance_root.empty() && knowledge_ &&
        knowledge_->get(current_game_id_, "case_sensitive", "true") == "false") {
        auto ow = current_instance_.path_for(engine::InstanceKind::Overwrite);
        QTimer::singleShot(0, this, [this, ow]() {
            if (engine::normalize_overwrite_casing(ow) > 0) {
                engine::Logger::instance().debug(
                    "Overwrite: healed case-insensitive directory duplicates in " +
                    ow.string());
            }
        });
    }

    // P5 (MO2 createOverwriteDirectories parity): pre-create the Overwrite
    // mapping root <overwrite>/<mods_subpath> so the game's first write finds
    // its target dir already present. Deferred + idempotent, never blocks load.
    if (!instance_root.empty() && knowledge_) {
        auto ow_root = current_instance_.path_for(engine::InstanceKind::Overwrite);
        auto ow_subpath =
            knowledge_->get(current_game_id_, "mods_subpath", "");
        if (!ow_subpath.empty()) {
            auto mapping_root = ow_root / ow_subpath;
            QTimer::singleShot(0, this, [this, mapping_root]() {
                std::error_code ec;
                std::filesystem::create_directories(mapping_root, ec);
                if (ec) {
                    engine::Logger::instance().warn(
                        "Overwrite: failed to pre-create mapping root " +
                        mapping_root.string() + ": " + ec.message());
                }
            });
        }
    }

    // Any in-flight conflict scan belongs to the previous instance: bump the
    // generation so its result is dropped when it lands, and discard queued
    // requests/invalidations for the old game. running_ stays true so the
    // worker thread never has more than one scan queued (they serialize on the
    // shared conflict cache file); the new game's first recompute queues behind
    // it and on_conflict_scan_finished() drains the queue on the stale result.
    conflict_scan_generation_ = conflict_scan_generation_ + 1;
    conflict_scan_pending_ = false;
    conflict_scan_pending_follow_ups_.clear();
    conflict_scan_active_follow_ups_.clear();
    conflict_invalidate_pending_.clear();
    // The same for any in-flight mod scan: it belongs to the previous
    // instance's mods dir, so its result must be dropped (load_mods_from_game
    // also bumps when it launches a fresh scan).
    mod_scan_generation_ = mod_scan_generation_ + 1;
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

        // Wire the freshly created Data tab: resolve real file paths and run
        // Open/Execute/Preview/Add-as-Executable/Mod Info/Hide from the
        // context menu, plus "Reveal in file manager" and refresh.
        wire_data_tab();

        // The Downloads tab was also freshly created by set_game: load its
        // manifest, point it at this instance's downloads dir (which starts
        // the directory watchdog) and connect its signals. Without this, a
        // switched instance shows a dead tab.
        wire_downloads_tab();

        // The Saves tab (if the game supports it) was freshly created too:
        // point it at the game's saves dir, connect refresh/delete, and run an
        // initial scan.
        wire_saves_tab();

        load_mods_from_game();
        // Plugin-DB disk load runs CONCURRENTLY with the mod scan (T6/P8.5):
        // the two independent startup loads overlap instead of the plugin DB
        // waiting for the scan to land and then reading the same disk
        // sequentially. refresh_plugins_tab() adopts the preload when it's
        // ready, and falls back to a synchronous read otherwise.
        launch_plugin_db_preload();
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

        // When an install targets an existing mod folder, ask the user how to
        // proceed (Merge/Replace/Rename/Cancel) instead of silently replacing.
        // The pipeline runs on a worker thread; ask_overwrite marshals the
        // modal dialog onto the main thread. Backup defaults to checked,
        // matching MO2's QueryOverwriteDialog::BACKUP_YES default.
        ctx.overwrite_query_cb = [this](const std::string& mod_name) {
            // The user dialog supersedes the progress popup (MO2 does the
            // same: the progress dialog is only visible while it can show
            // progress, not while a decision is pending).
            hide_install_progress();
            return ui::ask_overwrite(QString::fromStdString(mod_name),
                                     /*default_backup=*/true, this);
        };

        // A FOMOD archive opens the install wizard. It drives the
        // pipeline-owned FomodViewModel directly; ask_fomod marshals the modal
        // onto the main thread like ask_overwrite. Settings gate the
        // previous-choice restore and the image preview.
        ctx.fomod_query_cb =
            [this](const std::shared_ptr<engine::FomodViewModel>& view_model,
                   const std::filesystem::path& content_root,
                   const std::string& suggested_name,
                   const std::string& previous_choices) {
                hide_install_progress();
                auto& s = Settings::instance();
                return ui::ask_fomod(view_model, content_root, suggested_name,
                                     previous_choices,
                                     s.always_restore_fomod_choices(),
                                     s.show_fomod_images(), this);
            };

        // Non-FOMOD installs confirm the mod name before copying (MO2's
        // SimpleInstallDialog). The suggested name is the Downloads-tab display
        // name (typically the Nexus name); the dialog's dropdown also offers a
        // cleaned archive-stem derivation and the full archive filename.
        // Canceling aborts the install.
        ctx.name_query_cb =
            [this](const std::string& suggested_name,
                   const std::string& archive_filename) {
                hide_install_progress();
                return ui::ask_install_name(suggested_name, archive_filename, this);
            };

        // Set up deploy strategy
        ctx.deploy_prefix = knowledge_->get(current_game_id_, "deploy_prefix", "Data");
        auto inc_id = knowledge_->get(current_game_id_, "deploy_include_mod_id", "false");
        ctx.deploy_include_mod_id = (inc_id == "true");
        bool case_sensitive = knowledge_->get(current_game_id_, "case_sensitive", "true") != "false";
        std::unique_ptr<engine::DeploymentStrategy> deploy_strategy;
#ifdef GMM_PLATFORM_LINUX
        // Per-game deploy strategy. Default is Symlink (direct symlinks into
        // game_dir); a game opts out via the "deploy_strategy" knowledge key,
        // which selects the OverlayFS staging approach (when the host supports
        // it).
        const std::string deploy_strategy_name =
            engine::effective_deploy_strategy(current_instance_root_, *knowledge_,
                                              current_game_id_);
        if (deploy_strategy_name == engine::kDeployStrategyOverlayFs &&
            engine::OverlayFsLauncher::is_supported(overwrite_dir_path())) {
            // OverlayFS: deploy symlinks into staging dir (not game_dir)
            auto staging = current_instance_root_ / ".gmm_staging";
            ctx.staging_dir = staging;
            auto ovl_strat = std::make_unique<engine::OverlayFsDeployStrategy>(staging, case_sensitive);
            staging_dir_ = staging;
            deploy_strategy = std::move(ovl_strat);
            engine::Logger::instance().info("Deploy strategy: OverlayFS");
        } else
#endif
        {
            deploy_strategy = std::make_unique<engine::SymlinkStrategy>(case_sensitive);
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
    } else {
        // No game to load here. Any in-flight mod scan's result was just
        // dropped by the generation bump above, so clear the loading flag it
        // would otherwise leave stuck.
        loading_ = false;
    }

    // Restore saved app state now that current_instance_root_ is known
    restore_app_state();

    // Sync process tree checkbox/tree with restored state (overlay was created before restore)
    if (process_tree_checkbox_)
        process_tree_checkbox_->setChecked(show_process_tree_);
    if (process_tree_)
        process_tree_->setVisible(show_process_tree_);

    // The Downloads tab is wired (manifest, downloads dir + watchdog, signal
    // connections) by wire_downloads_tab() right after the tab is created.

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
        if (tool_id == QStringLiteral("loot")) {
            // LOOT is an advisory tool the engine drives itself: build a
            // LootRequest from the current plugin DB and run gmm_lootcli off
            // the UI thread (PLAN.md §7.1).
            run_loot_sort();
            return;
        }
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
        auto dl_dir = downloads_dir_path();
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

    // P1.3 event bus: mirror MO2 onModStateChanged. Fires on the UI thread
    // after the on-disk state change; a plugin handler must not block.
    engine::EventBus::instance().dispatch(
        engine::events::kModStateChanged,
        engine::json_obj({
            {"mod", mod_id.toStdString()},
            {"enabled", enabled ? "1" : "0"},
        }));
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
    // Only rows whose priority actually changed (marked by
    // renumber_priorities) are persisted — a reorder touches the moved rows,
    // never every mod's meta.ini. Ids that vanished (mod removed / renamed
    // after the mark) resolve to -1 and are skipped.
    auto dirty = mod_model_->dirty_priority_ids();
    for (const auto& id : dirty) {
        int i = mod_model_->priority_of(id);
        if (i < 0) continue;  // mod no longer present; nothing to persist

        // Persist priority to meta.ini for the row (Overwrite, separators, mods)
        if (!meta_dir.empty()) {
            auto meta = engine::ModMeta::load(meta_dir, id.toStdString());
            int old_priority = meta.priority();
            if (old_priority != i) {
                meta.set_priority(i);
                meta.save(meta_dir, id.toStdString());
                // P1.3 event bus: mirror MO2 onModMoved — fired only for real
                // moves, on the UI thread, after the priority persisted.
                if (old_priority >= 0 && !mods[i].is_overwrite &&
                    !mods[i].is_separator) {
                    engine::EventBus::instance().dispatch(
                        engine::events::kModMoved,
                        engine::json_obj({
                            {"mod", id.toStdString()},
                            {"from", std::to_string(old_priority)},
                            {"to", std::to_string(i)},
                        }));
                }
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
                auto mod_folder = resolve_mod_folder(id.toStdString(), mods_subpath);
                (void)engine::ModScanner::set_priority(*knowledge_, current_game_id_, mod_folder, i);
            }
        }
    }
    mod_model_->clear_dirty_priority_ids();
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
        if (mod.is_separator || mod.is_overwrite || mod.id == kOverwriteModId || mod.is_game_native) continue;

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

    // Create new ordered list: game-native (unmanaged) mods first (fixed top
    // band, in declared order), then the provider's sorted user mods.
    QVector<ui::ModEntry> new_order;
    for (const auto& mod : mod_model_->mods())
        if (mod.is_game_native) new_order.append(mod);

    // Add mods in sorted order
    for (const auto& folder : result.sorted_folders) {
        auto qfolder = QString::fromStdString(folder);
        if (mod_map.contains(qfolder)) {
            new_order.append(mod_map[qfolder]);
        }
    }

    // Add any mods not in the sorted result (shouldn't happen, but be safe)
    for (const auto& mod : mod_model_->mods()) {
        if (!mod.is_overwrite && !mod.is_game_native && std::find(result.sorted_folders.begin(), result.sorted_folders.end(), mod.id.toStdString()) == result.sorted_folders.end()) {
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

    // Any in-flight scan belongs to an older state (a refresh supersedes the
    // previous refresh; set_game_info bumps on instance switches too): bump
    // the generation so its result is dropped when it lands. The scan itself
    // runs on ModScanThread — the main thread does no directory walking here
    // (P8.2, THREADING.md §3.5/§3.6).
    mod_scan_generation_ = mod_scan_generation_ + 1;

    ui::ModScanRequest request = build_mod_scan_request();
    if (!mod_scan_thread_) {
        mod_scan_thread_ = new ui::ModScanThread(this);
        connect(mod_scan_thread_->worker(), &ui::ModScanWorker::finished,
                this, &MainWindow::on_mod_scan_finished, Qt::UniqueConnection);
    }
    mod_scan_thread_->start(std::move(request), mod_scan_generation_);
}

ui::ModScanRequest MainWindow::build_mod_scan_request() {
    ui::ModScanRequest request;
    request.knowledge = *knowledge_;  // snapshot — read-only after plugin registration
    request.game_id = current_game_id_;
    request.game_dir = current_game_dir_;
    request.instance_root = current_instance_root_;
    request.mods_dir = mods_dir_path();
    request.meta_dir = meta_dir_path();
    return request;
}

void MainWindow::launch_plugin_db_preload() {
    // Discard any preload state left over from a previous instance and bump
    // the generation so a still-running load's result is dropped when it lands.
    plugin_db_generation_ = plugin_db_generation_ + 1;
    preload_pending_ = false;
    preloaded_plugin_db_.reset();

    if (!knowledge_ || current_game_id_.empty() || current_game_dir_.empty()) return;
    const auto game_native = engine::native_plugins_csv(*knowledge_, current_game_id_);
    if (game_native.empty()) return;  // game declares no plugin hooks — nothing to preload

    ui::PluginDbLoadRequest request;
    request.game_dir = current_game_dir_;
    request.mods_dir = mods_dir_path();
    request.meta_dir = meta_dir_path();
    request.disable_mechanism = engine::disable_mechanism_for(*knowledge_, current_game_id_);
    request.game_native = game_native;

    preload_pending_ = true;
    preloaded_plugin_db_game_dir_ = current_game_dir_;
    if (!plugin_db_load_thread_) {
        plugin_db_load_thread_ = new ui::PluginDbLoadThread(this);
        connect(plugin_db_load_thread_->worker(), &ui::PluginDbLoadWorker::finished,
                this, &MainWindow::on_plugin_db_preloaded, Qt::UniqueConnection);
    }
    plugin_db_load_thread_->start(std::move(request), plugin_db_generation_);
}

void MainWindow::on_plugin_db_preloaded(engine::PluginDatabase db, quint64 generation) {
    if (generation != plugin_db_generation_ || !preload_pending_) {
        // Superseded by an instance switch or already consumed/superseded by a
        // synchronous fallback read — never adopt stale disk state.
        return;
    }
    preloaded_plugin_db_ = std::move(db);
}

bool MainWindow::adopt_preloaded_plugin_db() {
    if (!preload_pending_ || !preloaded_plugin_db_) return false;
    // The preload belongs to a different instance's game dir (paranoia; the
    // generation check above already covers switches) — refuse it.
    if (preloaded_plugin_db_game_dir_ != current_game_dir_) return false;
    plugins_db_ = std::move(*preloaded_plugin_db_);
    preloaded_plugin_db_.reset();
    preload_pending_ = false;
    return true;
}

void MainWindow::on_mod_scan_finished(ui::ModScanResult result, quint64 generation) {
    if (generation != mod_scan_generation_) {
        // Superseded (a newer refresh or instance switch launched another
        // scan): never apply a stale mod list. loading_ stays true — the newer
        // scan's result clears it when it lands.
        return;
    }

    auto& scanned = result.scanned;

    // Apply the per-instance column visibility (defaults on first run; Name is
    // always forced visible). The instance root is known now, so per-instance
    // settings resolve correctly across instance switches.
    restore_mod_column_visibility();

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
    engine::Logger::instance().debug(
        "on_mod_scan_finished: scan returned " + std::to_string(result.scanned.size()) +
        " mod(s), " + std::to_string(scanned.size()) + " after MERGED filter (gen=" +
        std::to_string(generation) + ")");

    // Add scanned mods before Overwrite (Overwrite stays last)
    for (const auto& mod : scanned) {
        auto id = QString::fromStdString(mod.folder_name);
        auto name = QString::fromStdString(mod.display_name);
        auto ver = QString::fromStdString(mod.version);
        if (mod.is_separator) {
            auto color = QString::fromStdString(mod.separator_color);
            mod_model_->add_separator(id, name, color);
        } else {
            mod_model_->add_mod(id, name, ver, mod.priority, mod.is_game_native,
                                mod.install_time, mod.changed_time);
            if (mod.is_fomod) {
                mod_model_->set_fomod(id, true);
            }
            if (mod.root_override) {
                mod_model_->set_root_override(id, true);
            }
            if (mod.invalid_data) {
                mod_model_->set_invalid_data(id, true);
            }
            if (mod.no_metadata) {
                mod_model_->set_no_metadata(id, true);
            }
            if (!mod.enabled) {
                mod_model_->toggle_mod(id);
            }
        }
    }

    // Load/create meta for each mod. (The one-time MO2 meta.ini import ran on
    // the worker thread as part of the scan; this only reads sidecars.)
    load_meta_for_mods();

    // Read persisted priority from meta.ini for ALL entries (including separators, Overwrite).
    // Mods without a persisted priority (e.g. freshly installed) get the bottom of the user
    // band - MO2's rule: a new mod gets the highest regular priority, just above the pinned
    // Overwrite/MERGED rows. set_priority() only writes the field; load_order() applies it.
    {
        auto meta_dir = meta_dir_path();
        if (!meta_dir.empty()) {
            // Game-native (unmanaged) mods own the top band, but a separator
            // may sit ABOVE it (its fold hides the native mods): that
            // separator keeps its persisted priority and the band shifts down
            // past it. Natives fill the remaining top slots in declared order.
            int native_priority = 0;
            std::set<int> sep_priorities;
            for (const auto& m : mod_model_->mods()) {
                if (!m.is_separator) continue;
                auto sep_meta = engine::ModMeta::load(meta_dir, m.id.toStdString());
                int sp = sep_meta.priority();
                if (sp >= 0) sep_priorities.insert(sp);
            }
            for (const auto& m : mod_model_->mods()) {
                if (!m.is_game_native) continue;
                while (sep_priorities.count(native_priority)) ++native_priority;
                mod_model_->set_priority(m.id, native_priority++);
            }

            // Non-pinned rows (natives + user mods) span priorities 0..regular-1;
            // a user mod without a persisted priority gets the highest one - just
            // above the pinned Overwrite/MERGED block (MO2's new-mod rule).
            int regular_rows = 0;
            for (const auto& m : mod_model_->mods()) {
                if (!m.is_overwrite && !m.is_merged) ++regular_rows;
            }
            int bottom_priority = std::max(0, regular_rows - 1);
            for (const auto& m : mod_model_->mods()) {
                if (m.is_game_native) continue;
                auto meta = engine::ModMeta::load(meta_dir, m.id.toStdString());
                int p = meta.priority();
                if (p < 0) p = bottom_priority;
                mod_model_->set_priority(m.id, p);
            }
        }
    }

    // Ensure MERGED pseudo-mod is present (after loading scanned mods, before sorting)
    mod_model_->ensure_merged_present();

    engine::Logger::instance().debug(
        "on_mod_scan_finished: model holds " + std::to_string(mod_model_->mods().size()) +
        " rows after adding scan results");

    loading_ = false;

    // Apply the per-instance nesting gate before restoring order/folds/links,
    // so load_order's fold restore and render decisions see the right mode.
    apply_nesting_setting();

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
        auto overwrite_dir = overwrite_dir_path();
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

    // Compute conflict stats for all mods (debounced entry; the scan runs off
    // the main thread per P8.1).
    recompute_conflicts();

    // Populate the Plugins tab from the (now loaded) mod list.
    refresh_plugins_tab();
}

void MainWindow::add_installed_mod(const std::string& folder_name) {
    if (folder_name.empty()) return;
    if (!knowledge_ || current_game_id_.empty()) return;

    // Scan just the one folder the install produced - not the whole mods dir.
    auto scanned = engine::ModScanner::scan_folder(
        *knowledge_, current_game_id_, mods_dir_path(), folder_name,
        current_instance_root_.empty() ? std::vector<std::filesystem::path>{}
                                       : std::vector<std::filesystem::path>{current_instance_root_});
    if (scanned.empty()) return;

    const auto& mod = scanned.front();

    // If the row already exists (Merge/Replace into an existing folder, or a
    // reinstall), don't add a duplicate - the files changed, so the conflict
    // and Data refreshes below still run.
    const auto id = QString::fromStdString(mod.folder_name);
    bool exists = false;
    for (const auto& m : mod_model_->mods()) {
        if (m.id == id) { exists = true; break; }
    }
    if (!exists) {
        auto name = QString::fromStdString(mod.display_name);
        auto ver = QString::fromStdString(mod.version);
        if (mod.is_separator) {
            mod_model_->add_separator(id, name,
                                      QString::fromStdString(mod.separator_color));
        } else {
            mod_model_->add_mod(id, name, ver, mod.priority, mod.is_game_native,
                                mod.install_time, mod.changed_time);
            if (mod.is_fomod) mod_model_->set_fomod(id, true);
            if (mod.root_override) mod_model_->set_root_override(id, true);
            if (mod.invalid_data) mod_model_->set_invalid_data(id, true);
            if (mod.no_metadata) mod_model_->set_no_metadata(id, true);
            if (!mod.enabled) mod_model_->toggle_mod(id);
        }
        // Persist the freshly assigned priority (MO2 bottom-of-band) and
        // separator ids, mirroring the full-load tail.
        sync_priorities();
        sync_separator_ids();
    } else {
        // Replace/merge into an existing folder changed its birth/write time,
        // so the Installation/Changed cells must follow (single-row refresh).
        mod_model_->set_timestamps(id, mod.install_time, mod.changed_time);
    }

    // Files changed regardless of whether the row is new: conflicts, Data tab
    // and Plugins tab all reflect the installed content. Unlike the full
    // recompute, the Data tab is refreshed incrementally: the engine's token
    // cache already limits the registry rescan to the new mod's files, and
    // apply_mod() merges only that mod's rows into the existing tree. The scan
    // runs off the main thread (P8.1); the incremental apply runs once the
    // freshly computed registry includes this mod.
    request_conflict_scan([this, folder_name]() {
        if (auto* dt = right_panel_->data_tab()) {
            std::string mods_subpath;
            std::string deploy_prefix;
            bool deploy_include_mod_id = false;
            if (knowledge_) {
                mods_subpath = knowledge_->get(current_game_id_, "mods_subpath", "");
                deploy_prefix = knowledge_->get(current_game_id_, "deploy_prefix", "Data");
                deploy_include_mod_id = knowledge_->get(
                    current_game_id_, "deploy_include_mod_id", "false") == "true";
            }
            dt->apply_mod(last_conflict_registry_, folder_name, mod_model_->mods(),
                          mod_model_->is_conflict_order_reversed(),
                          mods_dir_path(), current_game_mods_dir(),
                          current_game_dir_, mods_subpath, deploy_prefix,
                          deploy_include_mod_id);
        }
    });
    refresh_plugins_tab();

    // Update status bar mod count
    int count = 0;
    for (const auto& m : mod_model_->mods()) {
        if (!m.is_separator && !m.is_overwrite && m.enabled) ++count;
    }
    status_bar_->set_counter_value(count);

    engine::Logger::instance().debug("Added installed mod row: " + folder_name);
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
        return current_instance_.path_for(engine::InstanceKind::Mods);
    auto subpath = knowledge_ ? knowledge_->get(current_game_id_, "mods_subpath", "") : "";
    if (!subpath.empty())
        return current_game_dir_ / subpath;
    return current_game_dir_;
}

std::filesystem::path MainWindow::downloads_dir_path() const {
    if (current_instance_root_.empty()) return {};
    return current_instance_.path_for(engine::InstanceKind::Downloads);
}

std::filesystem::path MainWindow::cache_dir_path() const {
    if (current_instance_root_.empty()) return {};
    return current_instance_.path_for(engine::InstanceKind::Cache);
}

std::filesystem::path MainWindow::cache_thumbnails_dir_path() const {
    if (current_instance_root_.empty()) return {};
    return current_instance_.path_for(engine::InstanceKind::CacheThumbnails);
}

std::filesystem::path MainWindow::profiles_dir_path() const {
    if (current_instance_root_.empty()) return {};
    return current_instance_.path_for(engine::InstanceKind::Profiles);
}

std::filesystem::path MainWindow::overwrite_dir_path() const {
    if (current_instance_root_.empty()) return {};
    return current_instance_.path_for(engine::InstanceKind::Overwrite);
}

std::filesystem::path MainWindow::game_mygames_dir() const {
    if (!platform_ || !knowledge_ || current_game_id_.empty()) return {};
    auto id_str = knowledge_->get(current_game_id_, "steam_appid", "");
    if (id_str.empty()) return {};
    uint32_t appid = 0;
    try { appid = std::stoul(id_str); } catch (...) { return {}; }
    const auto documents = platform_->game_documents_dir(appid);
    if (documents.empty()) return {};
    auto sub = knowledge_->get(current_game_id_, "mygames_folder", "");
    if (sub.empty()) sub = current_game_name_.empty() ? current_game_id_ : current_game_name_;
    return documents / "My Games" / sub;
}

void MainWindow::load_meta_for_mods() {
    auto meta_dir = meta_dir_path();
    if (meta_dir.empty()) return;

    // Workshop ID pattern - used to detect Steam Workshop mods from folder names
    auto workshop_pattern = knowledge_->get(current_game_id_, "workshop_id_pattern", "");

    // Per-instance category DB (MO2's categories.dat/nexuscatmap.dat), loaded
    // once per call for the Category column. Same resolution the Categories tab
    // uses: [General] category CSV primary first, else the Nexus mapping.
    engine::Categories cats;
    if (!current_instance_root_.empty())
        cats = engine::Categories::load(current_instance_root_);

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
                                        QString::fromStdString(sid),
                                        QString::fromStdString(meta.source_page_url()));
        }

        // Category column: same resolution as the Categories tab — [General]
        // "category" CSV primary first, else the Nexus category mapping. Both
        // names come from the per-instance category DB.
        QString category_name;
        const auto csv = QString::fromStdString(meta.get("General", "category"));
        int primary = 0;
        const auto parts = csv.split(QLatin1Char(','), Qt::SkipEmptyParts);
        if (!parts.isEmpty()) primary = parts.first().toInt();
        if (primary <= 0) {
            const int nexus_id =
                QString::fromStdString(meta.get("Nexusmods", "nexuscategory")).toInt();
            if (nexus_id > 0) {
                if (const auto* cat = cats.category_for_nexus(nexus_id))
                    primary = cat->id;
            }
        }
        if (primary > 0) {
            if (const auto* cat = cats.find(primary))
                category_name = QString::fromStdString(cat->name);
        }
        if (!category_name.isEmpty())
            mod_model_->set_category(mod.id, category_name);

        // Update ModEntry with separator info from meta.ini
        auto sep_id = meta.separator_id();
        if (!sep_id.empty()) {
            mod_model_->set_separator_id(mod.id, QString::fromStdString(sep_id));
        }
    }
}

void MainWindow::restore_mod_column_visibility() {
    if (!mod_header_ || current_instance_root_.empty()) return;

    const auto key = QString::fromStdString(current_instance_root_.filename().string());
    const auto stored = Settings::instance().modlist_hidden_columns(key);
    const auto hidden_set = QSet<QString>(stored.cbegin(), stored.cend());

    for (int c = ModListModel::Name; c < ModListModel::ColumnCount; ++c) {
        const QString name = mod_column_name(c);
        if (name.isEmpty()) continue;
        // Name is hard-locked visible; everything else follows the stored set.
        const bool hidden = !mod_header_->is_locked(c) && hidden_set.contains(name);
        mod_header_->setSectionHidden(c, hidden);
    }
}

void MainWindow::recompute_conflicts() {
    if (loading_) return;  // load mutations are followed by an explicit scan
    if (!knowledge_ || current_game_id_.empty() || current_game_dir_.empty()) return;
    // Debounce: coalesce rapid toggle/reorder/refresh requests into one scan.
    if (conflict_debounce_timer_->isActive()) conflict_debounce_timer_->stop();
    conflict_debounce_timer_->start();
}

void MainWindow::start_conflict_scan() {
    request_conflict_scan([this]() { refresh_data_tab(); });
}

// Immediate (non-debounced) entry: used by the install path, whose follow-up
// must land after the freshly computed registry includes the new mod.
void MainWindow::request_conflict_scan(std::function<void()> follow_up) {
    std::vector<std::function<void()>> batch;
    if (follow_up) batch.push_back(std::move(follow_up));
    launch_conflict_scan_batch(std::move(batch));
}

void MainWindow::launch_conflict_scan_batch(std::vector<std::function<void()>> follow_ups) {
    if (conflict_scan_running_) {
        // One scan is already in flight. Queue a fresh one (snapshot is rebuilt
        // when it launches, so it reflects the newest state); its follow-ups
        // run after that newer scan lands.
        conflict_scan_pending_ = true;
        for (auto& f : follow_ups)
            if (f) conflict_scan_pending_follow_ups_.push_back(std::move(f));
        return;
    }

    // An immediate scan supersedes any pending debounce.
    conflict_debounce_timer_->stop();

    ui::ConflictScanRequest request = build_conflict_scan_request();
    engine::Logger::instance().debug(
        "conflict scan: " + std::to_string(request.mod_infos.size()) + " mod(s) for game " +
        current_game_id_ + " (mods_dir=" + request.mods_dir.string() +
        " extra=" + request.extra_mods_dir.string() + ")");
    if (request.mod_infos.empty()) {
        // Nothing enabled to scan: mirror the old compute_conflict_state()
        // early-return — the registry is cleared, follow-ups still run so the
        // Data tab (and any incremental install apply) empties.
        last_conflict_registry_.clear();
        for (auto& f : follow_ups)
            if (f) f();
        return;
    }

    conflict_scan_running_ = true;
    conflict_scan_active_follow_ups_ = std::move(follow_ups);
    conflict_scan_generation_ = conflict_scan_generation_ + 1;
    if (!conflict_scan_thread_) {
        conflict_scan_thread_ = new ui::ConflictScanThread(this);
        connect(conflict_scan_thread_->worker(), &ui::ConflictScanWorker::finished,
                this, &MainWindow::on_conflict_scan_finished, Qt::UniqueConnection);
    }
    conflict_scan_thread_->start(std::move(request), conflict_scan_generation_);
}

ui::ConflictScanRequest MainWindow::build_conflict_scan_request() {
    ui::ConflictScanRequest request;
    request.mods_dir = mods_dir_path();
    request.extra_mods_dir = current_game_mods_dir();
    request.cache_path = conflict_cache_path_;

    // Read per-game config from knowledge hooks (needed before mod_infos for overwrite priority)
    request.extensions_csv = knowledge_->get(current_game_id_, "conflict_extensions", "");
    request.ignored_csv = knowledge_->get(current_game_id_, "ignored_files", "");
    // Mod folders carry per-mod metadata files the manager itself writes
    // (meta.ini) or that the game reads (metadata.xml / disable marker).
    // Every mod folder has them, so exclude them from conflict counting.
    auto metadata_file = knowledge_->get(current_game_id_, "metadata_file", "meta.ini");
    auto disable_file = engine::disable_mechanism_for(*knowledge_, current_game_id_);
    for (const auto* f : {&metadata_file, &disable_file}) {
        if (f->empty()) continue;
        if (request.ignored_csv.find(*f) != std::string::npos) continue;
        if (!request.ignored_csv.empty()) request.ignored_csv += ",";
        request.ignored_csv += *f;
    }
    request.conflict_reversed =
        knowledge_->get(current_game_id_, "conflict_order_reversed", "") == "true";
    request.scan_dirs_csv = knowledge_->get(current_game_id_, "conflict_scan_dirs", "");

    // Collect mod info - only enabled mods affect the game
    for (const auto& mod : mod_model_->mods()) {
        if (mod.is_separator) continue;
        if (!mod.enabled && !mod.is_overwrite && !mod.is_merged) continue;
        if (mod.is_overwrite) {
            request.mod_infos.emplace_back(mod.id.toStdString(),
                                           request.conflict_reversed ? -1 : 999999);
            continue;
        }
        if (mod.is_merged) {
            request.mod_infos.emplace_back(mod.id.toStdString(),
                                           request.conflict_reversed ? 0 : 999998);
            continue;
        }
        request.mod_infos.emplace_back(mod.id.toStdString(), mod.priority);
    }

    request.invalidate = std::move(conflict_invalidate_pending_);
    conflict_invalidate_pending_.clear();
    return request;
}

void MainWindow::on_conflict_scan_finished(ui::ConflictScanResult result, quint64 generation) {
    if (generation != conflict_scan_generation_) {
        // Superseded (e.g. an instance switch bumped the generation while this
        // scan was in flight): never apply a stale result, but the worker is
        // idle now, so any queued requests for the newer state must still run.
        conflict_scan_running_ = false;
        if (conflict_scan_pending_) {
            conflict_scan_pending_ = false;
            auto batch = std::move(conflict_scan_pending_follow_ups_);
            conflict_scan_pending_follow_ups_.clear();
            launch_conflict_scan_batch(std::move(batch));
        }
        return;
    }
    conflict_scan_running_ = false;

    engine::Logger::instance().debug(
        "conflict scan finished: " + std::to_string(result.registry.size()) +
        " entries in registry (gen=" + std::to_string(generation) +
        " current_gen=" + std::to_string(conflict_scan_generation_) + ")");

    apply_conflict_results(result);

    auto follow_ups = std::move(conflict_scan_active_follow_ups_);
    conflict_scan_active_follow_ups_.clear();
    for (auto& f : follow_ups)
        if (f) f();
    reload_open_modinfo_dialog();

    // A request arrived mid-scan: launch the queued fresh scan now.
    if (conflict_scan_pending_) {
        conflict_scan_pending_ = false;
        auto batch = std::move(conflict_scan_pending_follow_ups_);
        conflict_scan_pending_follow_ups_.clear();
        launch_conflict_scan_batch(std::move(batch));
    }
}

void MainWindow::apply_conflict_results(const ui::ConflictScanResult& result) {
    // Push per-mod stats into the model
    for (const auto& [folder_name, cs] : result.stats) {
        mod_model_->set_conflict_stats(QString::fromStdString(folder_name), cs.wins, cs.losses);
    }
    // Zero out any stale stats for disabled mods (not fed to the engine)
    for (const auto& mod : mod_model_->mods()) {
        if (!mod.enabled && !mod.is_overwrite && !mod.is_merged && !mod.is_separator)
            mod_model_->set_conflict_stats(mod.id, 0, 0);
    }

    // "Redundant" mods: every file they provide is won by a higher-priority
    // owner, so nothing the mod provides actually takes effect.
    const auto& registry = result.registry;
    const bool conflict_reversed = result.conflict_reversed;
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
    for (const auto& [path, owners] : registry) {
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
    last_conflict_registry_ = result.registry;
}

void MainWindow::reload_open_modinfo_dialog() {
    if (!modinfo_dialog_) return;
    const QString id = modinfo_dialog_->current_mod_id();
    if (id.isEmpty()) return;
    for (const auto& m : mod_model_->mods()) {
        if (m.id == id) {
            modinfo_dialog_->reload_current(build_mod_info_data(m));
            return;
        }
    }
}

std::filesystem::path MainWindow::current_game_mods_dir() const {
    std::filesystem::path game_mods_dir;
    if (!current_game_dir_.empty() && knowledge_) {
        auto game_mods_subpath = knowledge_->get(current_game_id_, "mods_subpath", "");
        game_mods_dir = current_game_dir_;
        if (!game_mods_subpath.empty())
            game_mods_dir /= game_mods_subpath;
        // Only pass as extra dir if it differs from the instance mods dir
        if (game_mods_dir == mods_dir_path())
            game_mods_dir.clear();
    }
    return game_mods_dir;
}

void MainWindow::refresh_data_tab() {
    auto* dt = right_panel_->data_tab();
    if (!dt) return;

    engine::Logger::instance().debug(
        "refresh_data_tab: registry=" + std::to_string(last_conflict_registry_.size()) +
        " entries, game=" + current_game_id_);

    if (last_conflict_registry_.empty() || current_game_id_.empty()) {
        dt->clear_content();
        return;
    }

    auto mods_dir = mods_dir_path();
    auto game_mods_dir = current_game_mods_dir();

    std::string mods_subpath;
    std::string deploy_prefix;
    bool deploy_include_mod_id = false;
    if (knowledge_) {
        mods_subpath = knowledge_->get(current_game_id_, "mods_subpath", "");
        deploy_prefix = knowledge_->get(current_game_id_, "deploy_prefix", "Data");
        deploy_include_mod_id = knowledge_->get(
            current_game_id_, "deploy_include_mod_id", "false") == "true";
    }

    dt->show_data(last_conflict_registry_, mod_model_->mods(),
                  mod_model_->is_conflict_order_reversed(),
                  mods_dir, game_mods_dir,
                  current_game_dir_, mods_subpath, deploy_prefix,
                  deploy_include_mod_id);
}

void MainWindow::wire_data_tab() {
    auto* dt = right_panel_->data_tab();
    if (!dt || dt == data_tab_widget_) return;
    connect(dt, &ui::DataTab::open_requested, this, &MainWindow::on_data_open);
    connect(dt, &ui::DataTab::execute_requested, this, &MainWindow::on_data_execute);
    connect(dt, &ui::DataTab::preview_requested, this, &MainWindow::on_data_preview);
    connect(dt, &ui::DataTab::add_executable_requested,
            this, &MainWindow::on_data_add_executable);
    connect(dt, &ui::DataTab::open_mod_info_requested,
            this, [this](const QString& mod_id) { on_data_mod_info(mod_id); });
    connect(dt, &ui::DataTab::hide_requested, this, &MainWindow::on_data_hide);
    connect(dt, &ui::DataTab::refresh_requested, this, &MainWindow::recompute_conflicts);
    data_tab_widget_ = dt;
}

void MainWindow::on_data_open(const QString& file_path) {
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(file_path))) {
        QMessageBox::warning(this, tr("Open"),
                             tr("Failed to open:\n%1").arg(file_path));
    }
}

void MainWindow::on_data_execute(const QString& file_path, bool is_windows_exe,
                                 const QString& vfs_path) {
    // Every execute goes through the standard overlay-launch chain (the same
    // one the game and toolbar shortcuts use): it deploys enabled mods into
    // .gmm_staging and launches inside the overlay, so the tool sees the
    // merged view of every installed mod (MO2's plain Execute). The launchable
    // target is the merged Data-relative path; legacy absolute entries fall
    // back to their physical path, which the overlay also resolves.
    QString target = file_path;
    if (!vfs_path.isEmpty() && !current_game_dir_.empty()) {
        target = QString::fromStdString(
            (std::filesystem::weakly_canonical(current_game_dir_) /
             vfs_path.toStdString())
                .string());
    }
    (void)is_windows_exe;  // launch_with_executable derives it from the extension
    launch_with_executable(target, {});
}

void MainWindow::on_data_preview(const QString& file_path,
                                 const QStringList& provider_paths,
                                 const QStringList& provider_names) {
    if (!preview_window_)
        preview_window_ = new ui::preview::PreviewWindow(this);
    preview_window_->show_file(file_path, provider_paths, provider_names);
    preview_window_->show();
    preview_window_->raise();
    preview_window_->activateWindow();
}

void MainWindow::on_data_add_executable(const QString& file_path,
                                        const QString& default_name,
                                        const QString& physical_path) {
    auto* ec = right_panel_->exec_controls();
    if (!ec) return;

    // file_path is the merged-view (deploy-relative) path emitted by the Data
    // tab - stored verbatim. populate_executables / launch resolve it against
    // current_game_dir_, where the overlay mount makes it reachable.

    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Add as Executable"),
        tr("Name:"), QLineEdit::Normal, default_name, &ok);
    if (!ok || name.trimmed().isEmpty()) return;

    // Icon comes from the physical winning copy (DataRealPathRole) - the merged
    // path may not exist on disk until the first deploy this session. The
    // extraction is cached by basename so the entry keeps its icon across
    // restarts even with an empty staging dir.
    QIcon icon;
    if (!physical_path.isEmpty())
        icon = ui::extractExeIcon(physical_path, cache_thumbnails_dir_path());
    ec->add_executable(name, file_path, icon);
    save_executables();
}

ui::ModInfoData MainWindow::build_mod_info_data(const ModEntry& mod) {
    ui::ModInfoData data;
    data.id = mod.id;
    data.name = mod.name;
    data.version = mod.version;
    data.color = mod.separator_color;
    data.enabled = mod.enabled;
    data.is_separator = mod.is_separator;
    data.is_overwrite = mod.is_overwrite;
    data.is_game_native = mod.is_game_native;
    data.is_merged = mod.is_merged;
    data.priority = mod.priority;
    data.conflict_wins = mod.conflict_wins;
    data.conflict_losses = mod.conflict_losses;
    data.conflict_reversed = mod_model_->is_conflict_order_reversed();

    data.source_type = mod.source_type;
    data.source_id = mod.source_id;
    // From the plugin identity, not a knowledge hook (there is none named
    // "nexus_domain") - drives the Source-tab Visit-on-Nexus URL AND the
    // Nexus Refresh API call (games/{domain}/mods/{id}.json).
    data.nexus_domain = current_nexus_domain();

    // Sources the current game supports (download_sources knowledge, display
    // names like "Nexus") — gates which sub-tabs the Source tab shows.
    const auto sources_csv = knowledge_
        ? knowledge_->get(current_game_id_, "download_sources", "") : "";
    for (const auto& part :
         QString::fromStdString(sources_csv).split(',', Qt::SkipEmptyParts))
        data.supported_sources.append(part.trimmed());

    const auto mods_subpath = knowledge_
        ? knowledge_->get(current_game_id_, "mods_subpath", "") : "";
    data.data_subpath = QString::fromStdString(mods_subpath);

    std::filesystem::path mod_folder;
    if (mod.is_overwrite)
        mod_folder = overwrite_dir_path();
    else
        mod_folder = resolve_mod_folder(mod.id.toStdString(), mods_subpath);
    data.mod_dir = QDir(QString::fromStdString(mod_folder.string()));
    data.instance_root = QString::fromStdString(current_instance_root_.string());

    // Conflicts touching this mod (registry paths are mod-dir-relative, i.e.
    // what ConflictEngine walked with this mod folder as the root).
    for (const auto& [path, owners] : last_conflict_registry_) {
        bool is_owner = false;
        for (const auto& [owner, _] : owners)
            if (owner == mod.id.toStdString()) { is_owner = true; break; }
        if (!is_owner) continue;
        ui::ModInfoData::Owners owner_list;
        owner_list.reserve(owners.size());
        for (const auto& [owner, prio] : owners)
            owner_list.emplace_back(QString::fromStdString(owner), prio);
        data.conflicts.emplace_back(QString::fromStdString(path),
                                    std::move(owner_list));
    }

    // Persistence: GMM's canonical sidecar meta file (the same one the rest of
    // MainWindow reads/writes). Not the MO2-visible mods/<id>/meta.ini - tabs
    // use GMM-canonical keys ([Nexusmods] etc.) and rewriting an MO2-format
    // file with them would corrupt MO2 compatibility for imported mods.
    const auto meta_dir = meta_dir_path();
    data.load_meta = [meta_dir, mod_id = mod.id]() {
        return engine::ModMeta::load(meta_dir, mod_id.toStdString());
    };
    data.save_meta = [meta_dir, mod_id = mod.id](const engine::ModMeta& meta) {
        return meta.save(meta_dir, mod_id.toStdString());
    };

    // Actions wired to MainWindow.
    const QString mod_dir_str = QString::fromStdString(mod_folder.string());
    data.open_explorer = [mod_dir_str]() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(mod_dir_str));
    };
    data.open_file = [](const QString& path) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    };
    data.open_url = [](const QString& url) {
        QDesktopServices::openUrl(QUrl(url));
    };
    data.hide_file = [](const QString& abs, bool hide) {
        const std::filesystem::path p(abs.toStdString());
        return hide ? engine::hide_file(p) : engine::unhide_file(p);
    };
    data.set_mod_color = [this, mod_id = mod.id](const QColor& c) {
        if (c.isValid())
            mod_model_->set_mod_color(mod_id, c);
        else
            mod_model_->clear_mod_color(mod_id);
    };
    // Recompute is debounced + async now (P8.1); the dialog is reloaded with
    // the fresh conflict data from reload_open_modinfo_dialog() once the scan
    // lands, so no eager reload with stale data here.
    data.refresh_conflicts = [this]() { recompute_conflicts(); };
    data.delete_mod = [this, mod_id = mod.id, mods_subpath]() -> bool {
        if (!mods_subpath.empty() && !current_game_dir_.empty()) {
            auto mod_folder = mods_dir_path() / mod_id.toStdString();
            if (!engine::remove_path(mod_folder)) {
                engine::Logger::instance().error(
                    "Failed to move mod folder to trash: " + mod_folder.string());
            }
        }
        mod_model_->remove_mod(mod_id);
        // P1.3 event bus: mirror MO2 onModRemoved.
        engine::EventBus::instance().dispatch(
            engine::events::kModRemoved,
            engine::json_obj({{"mod", mod_id.toStdString()}}));
        return true;
    };

    // Live Nexus lookup for the Nexus tab's Refresh button.
    const QString domain = data.nexus_domain;
    const QString src_id = mod.source_id;
    data.fetch_nexus_info = [domain, src_id]() {
        auto* provider = dynamic_cast<engine::NexusProvider*>(
            engine::SourceRegistry::instance().provider_for("nexus"));
        if (!provider || domain.isEmpty() || src_id.isEmpty())
            return engine::ModInfoResult{};
        return provider->fetch_mod_info(domain.toStdString(),
                                        src_id.toStdString());
    };

    return data;
}

void MainWindow::on_data_mod_info(const QString& mod_id, int initial_tab) {
    std::vector<ui::ModInfoData> mods_data;
    mods_data.reserve(mod_model_->mods().size());
    int found = -1;
    for (const auto& mod : mod_model_->mods()) {
        if (mod.id == mod_id) found = static_cast<int>(mods_data.size());
        mods_data.push_back(build_mod_info_data(mod));
    }
    if (found < 0) return;

    ui::ModInfoDialog dlg(std::move(mods_data), found,
                          static_cast<ui::ModInfoTabId>(initial_tab), this);
    modinfo_dialog_ = &dlg;
    dlg.exec();
    modinfo_dialog_.clear();
}

void MainWindow::on_data_hide(const QString& file_path, const QString& mod_id, bool hide) {
    const auto p = std::filesystem::path(file_path.toStdString());
    const bool ok = hide ? engine::hide_file(p) : engine::unhide_file(p);
    if (!ok) {
        QMessageBox::warning(this, tr("Hide File"),
            tr("Failed to %1 the file.").arg(hide ? tr("hide") : tr("un-hide")));
        return;
    }
    // The rename happens inside a subdirectory (e.g. Data/...), which does NOT
    // change the mod root's quick token - the conflict cache would keep serving
    // the pre-rename file list and the tab would show the old name as a normal
    // file (with no real path, so no file menu). Drop the owning mod's cached
    // entry so the next scan re-scans it and surfaces the hidden/un-hidden
    // state. The invalidation is applied by the scan worker (before it reads
    // the cache), so it is only ever touched on the worker thread.
    conflict_invalidate_pending_.insert(mod_id.toStdString());
    recompute_conflicts();
}

void MainWindow::request_plugin_refresh() {
    if (loading_) return;  // the scan-finish tail refreshes plugins explicitly
    if (plugin_refresh_debounce_timer_->isActive())
        plugin_refresh_debounce_timer_->stop();
    plugin_refresh_debounce_timer_->start();
}

void MainWindow::refresh_plugins_tab() {
    if (loading_) return;
    auto* pt = right_panel_->plugins_tab();
    if (!pt || !knowledge_ || current_game_id_.empty() || current_game_dir_.empty()) {
        plugins_tab_widget_ = nullptr;
        plugin_owner_index_.clear();
        plugin_row_by_name_.clear();
        return;  // game without plugin support (or no tab yet)
    }
    if (pt != plugins_tab_widget_) {  // tab was recreated on game switch
        connect(pt, &ui::PluginsTab::toggle_requested, this, &MainWindow::on_plugin_toggle);
        connect(pt, &ui::PluginsTab::reorder_requested, this, &MainWindow::on_plugin_reorder);
        connect(pt, &ui::PluginsTab::lock_requested, this, &MainWindow::on_plugin_lock);
        connect(pt, &ui::PluginsTab::refresh_requested, this, [this]() {
            refresh_plugins_tab();
            // set_plugins() rebuilds the rows and clears row-hidden states;
            // re-apply the active text filter so rows and counter stay
            // consistent with the filter box.
            if (right_panel_) right_panel_->reapply_current_filter();
        });
        connect(pt->table(), &QTableWidget::itemSelectionChanged,
                this, &MainWindow::on_plugin_selection_changed);
        plugins_tab_widget_ = pt;
    }

    const auto game_native = engine::native_plugins_csv(*knowledge_, current_game_id_);
    if (game_native.empty()) {  // tab exists but the module declares no plugin hooks
        plugins_db_ = engine::PluginDatabase{};
        plugin_owner_index_.clear();
        plugin_row_by_name_.clear();
        pt->set_plugins({});
        return;
    }

    // T6: when the concurrently-preloaded DB (launch_plugin_db_preload) is
    // ready, adopt it and skip the synchronous disk read entirely. Otherwise
    // fall back to it — and drop the pending preload so a late-landing result
    // can't clobber the fresher synchronous read.
    bool adopted = adopt_preloaded_plugin_db();
    if (!adopted && preload_pending_) {
        preload_pending_ = false;
        preloaded_plugin_db_.reset();
    }
    if (!adopted) {
        const auto disable_mechanism = engine::disable_mechanism_for(*knowledge_, current_game_id_);
        plugins_db_.refresh(current_game_dir_, mods_dir_path(), meta_dir_path(),
                            disable_mechanism, game_native);
        plugins_db_.load_creation_club(current_game_dir_);
        plugins_db_.sort_load_order();
    }

    // A persisted profile is the source of truth once it exists; only a first
    // run (no profile yet) enables everything and writes it.
    const auto profiles_dir = profiles_dir_path();
    bool applied = false;
    if (!profiles_dir.empty()) {
        bool repaired = false;
        applied = plugins_db_.load_profile(profiles_dir, current_profile_name_, &repaired);
        if (repaired)  // core plugins were found below user ones - persist the heal
            plugins_db_.save_profile(profiles_dir, current_profile_name_);
    }
    if (!applied) {
        plugins_db_.set_all_enabled();
        plugins_db_.set_missing_masters();
        if (!profiles_dir.empty())
            plugins_db_.save_profile(profiles_dir, current_profile_name_);
    }
    plugins_db_.generate_mod_indexes();
    if (plugin_loader_)  // plugin-supplied diagnostics land in the tooltip
        plugin_loader_->collect_diagnostics(current_game_id_, plugins_db_);
    pt->set_plugins(plugins_db_.plugins());
    rebuild_plugin_highlight_index();
    // Rows and the selection indexes were rebuilt; re-apply any highlights the
    // user still has active.
    on_mod_selection_changed();
    on_plugin_selection_changed();

    // P1.3 event bus: mirror MO2 onRefreshed (plugin list rebuilt).
    engine::EventBus::instance().dispatch(
        engine::events::kPluginListRefreshed, "{}");
}

void MainWindow::run_loot_sort() {
    if (loading_ || current_game_id_.empty() || current_game_dir_.empty() || !knowledge_)
        return;

    const std::string loot_game_id =
        knowledge_->get(current_game_id_, "loot_game_id", "");
    if (loot_game_id.empty()) {
        if (status_bar_)
            status_bar_->set_status(tr("This game has no LOOT support"));
        return;
    }

    // A fresh plugin DB: mods may have changed since the last render, and the
    // request must carry each plugin's current winning path.
    refresh_plugins_tab();

    engine::LootRequest request;
    request.game_id = current_game_id_;
    request.loot_game_id = loot_game_id;
    request.masterlist_repo =
        knowledge_->get(current_game_id_, "loot_masterlist_repo", loot_game_id);
    request.game_dir = current_game_dir_;
    request.profile_dir = profiles_dir_path() / current_profile_name_;
    request.platform = platform_;
    for (const auto& p : plugins_db_.plugins()) {
        request.plugins.push_back({p.name, p.full_path});
    }
    if (request.plugins.empty()) {
        if (status_bar_) status_bar_->set_status(tr("No plugins to sort"));
        return;
    }

    // gmm_lootcli ships next to the manager binary (build tree and install
    // tree alike); fall back to a PATH search.
    request.cli_path =
        QCoreApplication::applicationDirPath().toStdString() + "/gmm_lootcli";
    if (!std::filesystem::is_regular_file(request.cli_path)) {
        if (const char* path_env = std::getenv("PATH")) {
            std::istringstream ss(path_env);
            std::string token;
            while (std::getline(ss, token, ':')) {
                auto candidate = std::filesystem::path(token) / "gmm_lootcli";
                if (std::filesystem::is_regular_file(candidate)) {
                    request.cli_path = candidate;
                    break;
                }
            }
        }
    }

    if (!loot_sort_thread_) {
        loot_sort_thread_ = new ui::LootSortThread(this);
        connect(loot_sort_thread_->worker(), &ui::LootSortWorker::progress,
                this, &MainWindow::on_loot_progress, Qt::UniqueConnection);
        connect(loot_sort_thread_->worker(), &ui::LootSortWorker::finished,
                this, &MainWindow::on_loot_finished, Qt::UniqueConnection);
    }
    if (status_bar_)
        status_bar_->set_status(tr("Sorting load order with LOOT…"));
    loot_sort_thread_->start(std::move(request));
}

void MainWindow::on_loot_progress(int stage, const QString&) {
    static const QStringList kStageNames = {
        QString(),                                  // 0 - none
        tr("Checking masterlist…"),                 // 1
        tr("Updating masterlist…"),                 // 2
        tr("Loading masterlists…"),                 // 3
        tr("Reading plugins…"),                     // 4
        tr("Sorting plugins…"),                     // 5
        tr("Writing load order…"),                  // 6
        tr("Parsing LOOT messages…"),               // 7
        tr("Load order sorted"),                    // 8
    };
    if (!status_bar_) return;
    if (stage >= 0 && stage < kStageNames.size() && !kStageNames.at(stage).isEmpty())
        status_bar_->set_status(kStageNames.at(stage));
}

void MainWindow::on_loot_finished(engine::LootResult result) {
    if (!result.ok) {
        engine::Logger::instance().warn("LOOT sort failed: " + result.error);
        if (status_bar_)
            status_bar_->set_status(tr("LOOT sort failed: %1")
                                        .arg(QString::fromStdString(result.error)));
        return;
    }

    std::string err;
    if (!plugins_db_.apply_load_order(result.sorted_names, &err)) {
        if (status_bar_)
            status_bar_->set_status(tr("LOOT sort could not be applied: %1")
                                        .arg(QString::fromStdString(err)));
        return;
    }
    plugins_db_.save_profile(profiles_dir_path(), current_profile_name_);
    refresh_plugins_tab();
    if (status_bar_)
        status_bar_->set_status(tr("Load order sorted by LOOT (%1 plugins)")
                                    .arg(result.sorted_names.size()));
}

void MainWindow::on_plugin_toggle(const std::string& name, bool enabled) {
    std::string err;
    if (!plugins_db_.set_enabled(name, enabled, &err)) {
        auto* pt = right_panel_->plugins_tab();
        if (pt) pt->sync_enabled(plugins_db_.plugins());
        if (!err.empty())
            QMessageBox::warning(this, tr("Plugins"), QString::fromStdString(err));
        return;
    }
    plugins_db_.save_profile(profiles_dir_path(), current_profile_name_);
    auto* pt = right_panel_->plugins_tab();
    if (pt) pt->sync_enabled(plugins_db_.plugins());
    // P1.3 event bus: mirror MO2 onPluginStateChanged.
    engine::EventBus::instance().dispatch(
        engine::events::kPluginStateChanged,
        engine::json_obj({
            {"plugin", name},
            {"enabled", enabled ? "1" : "0"},
        }));
}

void MainWindow::on_plugin_reorder(int from_row, int to_row) {
    // Capture the moved plugin's name before the reorder so the event carries
    // it (refresh_plugins_tab() rebuilds rows right after the move).
    std::string moved_name;
    const auto& plugins_before = plugins_db_.plugins();
    if (from_row >= 0 && from_row < static_cast<int>(plugins_before.size()))
        moved_name = plugins_before[static_cast<size_t>(from_row)].name;

    std::string err;
    if (!plugins_db_.move_plugin(from_row, to_row, &err)) {
        if (!err.empty())
            QMessageBox::warning(this, tr("Plugins"), QString::fromStdString(err));
        return;
    }
    plugins_db_.save_profile(profiles_dir_path(), current_profile_name_);
    refresh_plugins_tab();  // repopulate: new order + recomputed priorities/indexes
    // P1.3 event bus: mirror MO2 onPluginMoved.
    engine::EventBus::instance().dispatch(
        engine::events::kPluginMoved,
        engine::json_obj({
            {"plugin", moved_name},
            {"from", std::to_string(from_row)},
            {"to", std::to_string(to_row)},
        }));
}

void MainWindow::on_plugin_lock(const std::string& name, bool locked) {
    std::string err;
    if (!plugins_db_.set_locked(name, locked, &err)) {
        if (!err.empty())
            QMessageBox::warning(this, tr("Plugins"), QString::fromStdString(err));
        return;
    }
    plugins_db_.save_profile(profiles_dir_path(), current_profile_name_);
    refresh_plugins_tab();  // repopulate: lock emblem + drag flags re-applied
}

void MainWindow::rebuild_plugin_highlight_index() {
    plugin_owner_index_.clear();
    plugin_row_by_name_.clear();
    const auto& plugins = plugins_db_.plugins();
    plugin_row_by_name_.reserve(static_cast<int>(plugins.size()));
    for (size_t i = 0; i < plugins.size(); ++i) {
        const auto& p = plugins[i];
        plugin_row_by_name_.insert(QString::fromStdString(p.name), static_cast<int>(i));
        if (!p.owner_mod.empty())
            plugin_owner_index_[QString::fromStdString(p.owner_mod)]
                .append(QString::fromStdString(p.name));
    }
}

void MainWindow::on_mod_selection_changed() {
    const auto& mods = mod_model_->mods();
    const auto rows = mod_view_->selectionModel()->selectedRows();

    // Conflict-highlight union across the whole selection (MO2
    // refreshMarkersAndPlugins -> setOverwriteMarkers parity). Independent of
    // the plugins tab, so run before its early return.
    QSet<QString> conflict_ids;
    for (const auto& idx : rows) {
        const int r = idx.row();
        if (r < 0 || r >= mods.size()) continue;
        const auto& m = mods[r];
        if (m.is_separator || m.is_overwrite || m.is_merged) continue;
        conflict_ids.insert(m.id);
    }
    mod_model_->set_selected_mods(conflict_ids);

    auto* pt = right_panel_ ? right_panel_->plugins_tab() : nullptr;
    if (!pt || plugin_row_by_name_.isEmpty()) return;

    QVector<QString> contained;
    QSet<QString> seen_contained;
    contained.reserve(plugin_row_by_name_.size());

    for (const auto& idx : rows) {
        const int r = idx.row();
        if (r < 0 || r >= mods.size()) continue;
        const auto& m = mods[r];
        if (m.is_separator || m.is_overwrite || m.is_merged) continue;

        // Mods own the plugins whose owner_mod matches their id (MO2's
        // highlightPlugins, via DirectoryEntry origin - GMM's owner_mod is the
        // winning origin already).
        const auto it = plugin_owner_index_.constFind(m.id);
        if (it != plugin_owner_index_.constEnd()) {
            for (const auto& name : it.value()) {
                if (seen_contained.contains(name)) continue;
                seen_contained.insert(name);
                contained.append(name);
            }
        }
        // Unmanaged (game-native / stray) mod: its plugin is the game file with
        // the same name, as long as no mod wins that file.
        if (m.is_game_native) {
            const auto nit = plugin_row_by_name_.constFind(m.id);
            if (nit != plugin_row_by_name_.constEnd()) {
                const auto& p = plugins_db_.plugins()[nit.value()];
                if (p.owner_mod.empty() && !seen_contained.contains(m.id)) {
                    seen_contained.insert(m.id);
                    contained.append(m.id);
                }
            }
        }
    }
    pt->set_contained_plugins(contained);
}

void MainWindow::on_plugin_selection_changed() {
    auto* pt = right_panel_ ? right_panel_->plugins_tab() : nullptr;
    if (!pt) return;

    const QStringList selected = pt->selected_plugin_names();
    QSet<QString> highlighted_mods;
    QVector<QString> masters;
    QSet<QString> seen_masters;

    for (const auto& name : selected) {
        const auto it = plugin_row_by_name_.constFind(name);
        if (it == plugin_row_by_name_.constEnd()) continue;
        const auto& p = plugins_db_.plugins()[it.value()];
        // Owning mod: owner_mod, or the unmanaged mod row for game-owned files
        // (MO2's plugin-list selection -> setHighlightedMods).
        const QString owner =
            p.owner_mod.empty() ? name : QString::fromStdString(p.owner_mod);
        highlighted_mods.insert(owner);
        // Masters of the selected plugin render plugin_list_master.
        for (const auto& master : p.masters) {
            const QString m = QString::fromStdString(master);
            if (!plugin_row_by_name_.contains(m) || seen_masters.contains(m)) continue;
            seen_masters.insert(m);
            masters.append(m);
        }
    }
    mod_model_->set_highlighted_mods(highlighted_mods);
    pt->set_master_plugins(masters);
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
            // MO2 ModListContextMenu::addOverwriteActions. The move/sync/clear
            // actions only make sense when Overwrite has content; Open in
            // Explorer and Information always apply. Gating mirrors MO2's
            // `QDir(...).count() > 2` via overwrite_is_empty().
            auto ow_subpath = knowledge_
                ? knowledge_->get(current_game_id_, "mods_subpath", "") : std::string();
            const bool has_content =
                !engine::overwrite_is_empty(overwrite_dir_path(), ow_subpath);

            auto* sync_act = menu.addAction(engine::IconManager::instance().resolve_icon("merge"),
                tr("Sync to Mods..."),
                this, [this]() { sync_overwrite_to_mods(); });
            auto* create_act = menu.addAction(engine::IconManager::instance().resolve_icon("document-new"),
                tr("Create Mod..."),
                this, [this]() { create_mod_from_overwrite(); });
            auto* move_act = menu.addAction(engine::IconManager::instance().resolve_icon("go-down"),
                tr("Move content to Mod..."),
                this, [this]() { move_overwrite_content_to_mod(); });
            auto* clear_act = menu.addAction(engine::IconManager::instance().resolve_icon("edit-clear"),
                tr("Clear Overwrite..."),
                this, [this]() { clear_overwrite(); });
            for (auto* act : {sync_act, create_act, move_act, clear_act})
                act->setEnabled(has_content);

            menu.addAction(engine::IconManager::instance().resolve_icon("folder"), tr("Open in File Manager"),
                this, [this]() { open_overwrite_in_file_manager(); });

            menu.addSeparator();
            menu.addAction(engine::IconManager::instance().resolve_icon("dialog-information"), tr("Information..."),
                this, [this]() { show_overwrite_info_dialog(); });

            menu.exec(mod_view_->viewport()->mapToGlobal(pos));
            return;
        }

        if (entry.is_separator) {
            // MO2's separator context menu (modlistcontextmenu.cpp:381-409):
            // Rename (inline edit) / Remove / Select Color / Reset Color.
            menu.addAction(engine::IconManager::instance().resolve_icon("document-edit"), tr("Rename Separator..."),
                this, [this, row]() { rename_mod_inline(row); });
            menu.addAction(engine::IconManager::instance().resolve_icon("edit-delete"), tr("Remove Separator..."),
                this, [this, row]() { delete_separator(row); });
            menu.addSeparator();
            menu.addAction(engine::IconManager::instance().resolve_icon("color-picker"), tr("Select Color..."),
                this, [this]() { select_color_for_selected(); });
            if (!entry.separator_color.isEmpty()) {
                menu.addAction(engine::IconManager::instance().resolve_icon("edit-clear"), tr("Reset Color"),
                    this, [this]() { reset_color_for_selected(); });
            }
            menu.exec(mod_view_->viewport()->mapToGlobal(pos));
            return;
        }

        // --- Mod rows below ---
        auto sel = mod_view_->selectionModel()->selectedRows();
        bool multi = sel.size() > 1;

        if (multi) {
            menu.addAction(engine::IconManager::instance().resolve_icon("dialog-ok"), tr("Enable Selected"),
                this, [this]() { toggle_selected_mods(true); });
            menu.addAction(engine::IconManager::instance().resolve_icon("dialog-cancel"), tr("Disable Selected"),
                this, [this]() { toggle_selected_mods(false); });
            // Tweaks submenu: applies to every selected mod. Checked only when
            // ALL of them share the state; clicking applies the inverse.
            {
                QList<int> rows;
                bool any_on = false;
                bool any_off = false;
                for (const auto& si : sel) {
                    if (si.row() < 0 || si.row() >= mod_model_->mods().size()) continue;
                    const auto& m = mod_model_->mods()[si.row()];
                    if (m.is_separator || m.is_overwrite || m.is_merged || m.is_game_native)
                        continue;
                    rows << si.row();
                    if (m.root_override) any_on = true; else any_off = true;
                }
                auto* tweaks = menu.addMenu(engine::IconManager::instance().resolve_icon("preferences-other"), tr("Tweaks"));
                auto* root_act = tweaks->addAction(tr("Treat mod as root dir"));
                root_act->setCheckable(true);
                const bool all_on = any_on && !any_off;
                root_act->setChecked(all_on);
                root_act->setEnabled(!rows.isEmpty());
                connect(root_act, &QAction::triggered, this, [this, rows, all_on]() {
                    toggle_root_override(rows, !all_on);
                });
            }
            // Send to... submenu (MO2 modlistcontextmenu.cpp:325 sends all
            // selected mods to one separator): applies to every selected mod
            // row, skipping separators/pinned rows. Relative order is preserved
            // by move_mods_to_separator.
            {
                QList<int> rows;
                bool any_seps = false;
                for (const auto& m : mod_model_->mods())
                    if (m.is_separator) { any_seps = true; break; }
                for (const auto& si : sel) {
                    if (si.row() < 0 || si.row() >= mod_model_->mods().size()) continue;
                    const auto& m = mod_model_->mods()[si.row()];
                    if (m.is_separator || m.is_overwrite || m.is_merged || m.is_game_native)
                        continue;
                    rows << si.row();
                }
                auto* send_to = menu.addMenu(engine::IconManager::instance().resolve_icon("view-sort"), tr("Send to..."));
                auto* sep_act = send_to->addAction(engine::IconManager::instance().resolve_icon("view-sort"), tr("Separator..."),
                    this, [this, rows]() { send_to_separator(rows); });
                sep_act->setEnabled(any_seps && !rows.isEmpty());
            }
            menu.addSeparator();
            menu.addAction(engine::IconManager::instance().resolve_icon("edit-delete"), tr("Remove"),
                this, [this]() { remove_selected_mods(); });
            menu.exec(mod_view_->viewport()->mapToGlobal(pos));
            return;
        }

        // Single mod - full menu
        auto mod_id = entry.id;

        // Send to... submenu (MO2 modlistcontextmenu.cpp:285-338): priority
        // moves + the separator picker. The separator picker opens the shared
        // ListDialog (MO2 sendModsToSeparator, listdialog.ui) instead of an
        // inline submenu entry per separator — a submenu with many separators
        // (or long names) grew to cover the whole screen.
        auto* send_to = menu.addMenu(engine::IconManager::instance().resolve_icon("view-sort"), tr("Send to..."));
        send_to->addAction(engine::IconManager::instance().resolve_icon("go-top"), tr("Send to Highest Priority"),
            this, [this, mod_id]() { send_to_highest_priority(mod_id); });
        send_to->addAction(engine::IconManager::instance().resolve_icon("go-bottom"), tr("Send to Lowest Priority"),
            this, [this, mod_id]() { send_to_lowest_priority(mod_id); });
        bool any_seps = false;
        for (const auto& m : mod_model_->mods())
            if (m.is_separator) { any_seps = true; break; }
        auto* sep_act = send_to->addAction(engine::IconManager::instance().resolve_icon("view-sort"), tr("Separator..."),
            this, [this, row]() { send_to_separator(QList<int>{row}); });
        sep_act->setEnabled(any_seps);
        if (!entry.separator_id.isEmpty() && mod_model_->has_conflicts_within_separator(mod_id)) {
            send_to->addAction(engine::IconManager::instance().resolve_icon("go-up"), tr("Send to Highest in Separator"),
                this, [this, mod_id]() { send_to_highest_in_separator(mod_id); });
            send_to->addAction(engine::IconManager::instance().resolve_icon("go-down"), tr("Send to Lowest in Separator"),
                this, [this, mod_id]() { send_to_lowest_in_separator(mod_id); });
        }

        menu.addAction(engine::IconManager::instance().resolve_icon("list-add"), tr("Create Separator"),
            this, [this, row]() { create_separator_at_row(row); });

        // MO2's "Ignore missing data" (modlistcontextmenu + modlistviewactions
        // ignoreMissingData): offered only on flagged rows (no valid game data
        // and/or no manager metadata). Persists [General] validated=true in the
        // mod's own meta.ini so the flags stay cleared on rescan.
        if (entry.invalid_data || entry.no_metadata) {
            menu.addSeparator();
            menu.addAction(engine::IconManager::instance().resolve_icon("dialog-ok"), tr("Ignore missing data"),
                this, [this, mod_id]() {
                    auto folder = mods_dir_path() / mod_id.toStdString();
                    if (engine::ModScanner::mark_validated(folder)) {
                        mod_model_->set_invalid_data(mod_id, false);
                        mod_model_->set_no_metadata(mod_id, false);
                    }
                });
        }

        menu.addSeparator();
        menu.addAction(engine::IconManager::instance().resolve_icon("dialog-ok"), tr("Enable Selected"),
            this, [this]() { toggle_selected_mods(true); });
        menu.addAction(engine::IconManager::instance().resolve_icon("dialog-cancel"), tr("Disable Selected"),
            this, [this]() { toggle_selected_mods(false); });

        menu.addSeparator();
        menu.addAction(engine::IconManager::instance().resolve_icon("document-edit"), tr("Rename Mod..."),
            this, [this, row]() { rename_mod_inline(row); });

        // Tweaks submenu - per-mod deploy options (MO2's per-mod tweaks).
        {
            auto* tweaks = menu.addMenu(engine::IconManager::instance().resolve_icon("preferences-other"), tr("Tweaks"));
            auto* root_act = tweaks->addAction(tr("Treat mod as root dir"));
            root_act->setCheckable(true);
            root_act->setChecked(entry.root_override);
            root_act->setEnabled(!entry.is_separator && !entry.is_overwrite &&
                                 !entry.is_merged && !entry.is_game_native);
            root_act->setStatusTip(tr("Deploy this mod's files to the game root "
                                      "instead of the data dir"));
            connect(root_act, &QAction::triggered, this, [this, row]() {
                toggle_root_override({row}, !mod_model_->mods()[row].root_override);
            });
        }

        menu.addSeparator();
        if (!entry.source_type.isEmpty()) {
            auto src = source_visit_info(entry.source_type, entry.source_id,
                                         entry.source_page_url);
            if (!src.label.isEmpty()) {
                auto* visit_act = menu.addAction(engine::IconManager::instance().resolve_icon("text-html"), src.label,
                    this, [this, src]() {
                        if (!src.url.isEmpty())
                            QDesktopServices::openUrl(QUrl(src.url));
                    });
                visit_act->setEnabled(!src.url.isEmpty());
            }
        }
        menu.addAction(engine::IconManager::instance().resolve_icon("folder"), tr("Open in File Manager"),
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
        menu.addAction(engine::IconManager::instance().resolve_icon("edit-delete"), tr("Remove"),
            this, [this]() { remove_selected_mods(); });

        // MO2 puts Information last (modlistcontextmenu.cpp:267-273), the
        // default action after all per-type actions.
        menu.addSeparator();
        menu.addAction(engine::IconManager::instance().resolve_icon("dialog-information"), tr("Information..."),
            this, [this, mod_id]() { on_data_mod_info(mod_id); });

        menu.exec(mod_view_->viewport()->mapToGlobal(pos));
    });
}

void MainWindow::clear_overwrite() {
    auto reply = QMessageBox::question(this, tr("Clear Overwrite"),
        tr("Remove all files from the Overwrite folder? Deleted files go to the system trash."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    if (current_instance_root_.empty()) return;
    auto overwrite_dir = overwrite_dir_path();
    auto mods_subpath = knowledge_ ? knowledge_->get(current_game_id_, "mods_subpath", "") : std::string();
    auto cleared = engine::clear_overwrite(overwrite_dir, mods_subpath);
    if (cleared > 0) {
        engine::Logger::instance().debug("Overwrite cleared (" +
            std::to_string(cleared) + " file(s))");
        QMessageBox::information(this, tr("Overwrite"), tr("Overwrite folder cleared."));
    } else {
        QMessageBox::warning(this, tr("Overwrite"), tr("Failed to clear Overwrite folder."));
    }
}

void MainWindow::create_mod_from_overwrite() {
    if (current_instance_root_.empty()) return;
    auto overwrite_dir = overwrite_dir_path();
    auto mods_subpath = knowledge_ ? knowledge_->get(current_game_id_, "mods_subpath", "") : std::string();
    if (mods_subpath.empty()) return;

    if (engine::overwrite_is_empty(overwrite_dir)) {
        QMessageBox::information(this, tr("Create Mod"), tr("Overwrite folder is empty."));
        return;
    }

    bool ok;
    auto name = QInputDialog::getText(this, tr("Create Mod from Overwrite"),
        tr("Mod name:"), QLineEdit::Normal, QString(), &ok);
    if (!ok || name.isEmpty()) return;

    auto mod_dir = mods_dir_path() / name.toStdString();
    auto moved = engine::move_overwrite_to_mod(overwrite_dir, mod_dir, mods_subpath);
    if (moved) {
        // Write the game's metadata file so ModScanner picks the mod up.
        auto metadata_file = knowledge_->get(current_game_id_, "metadata_file", "meta.ini");
        engine::ModMeta::write_game_metadata(mod_dir, metadata_file,
                                             name.toStdString(), "1.0", "0");
        auto id = name;
        mod_model_->add_mod(id, name, "");
        engine::Logger::instance().debug("Promote Overwrite to mod: " + name.toStdString());
        QMessageBox::information(this, tr("Create Mod"),
            tr("Overwrite contents promoted to mod: %1").arg(name));
    } else {
        QMessageBox::warning(this, tr("Create Mod"), tr("Failed to promote Overwrite files."));
    }
}

void MainWindow::move_overwrite_content_to_mod() {
    if (current_instance_root_.empty()) return;
    auto overwrite_dir = overwrite_dir_path();
    auto mods_subpath = knowledge_ ? knowledge_->get(current_game_id_, "mods_subpath", "") : std::string();
    if (mods_subpath.empty()) return;
    if (engine::overwrite_is_empty(overwrite_dir)) {
        QMessageBox::information(this, tr("Move content"), tr("Overwrite folder is empty."));
        return;
    }

    // MO2 moveOverwriteContentToExistingMod: picker excludes separators /
    // foreign (game-native) / Overwrite / merged mods.
    std::vector<std::pair<std::string, std::string>> mods;
    for (const auto& m : mod_model_->mods()) {
        if (m.is_separator || m.is_overwrite || m.is_merged || m.is_game_native) continue;
        mods.emplace_back(m.id.toStdString(), m.name.toStdString());
    }
    if (mods.empty()) {
        QMessageBox::information(this, tr("Move content"), tr("No mods available."));
        return;
    }

    MoveToModDialog dialog(mods, this);
    if (dialog.exec() != QDialog::Accepted) return;
    auto folder = dialog.selected_folder();
    if (folder.empty()) return;

    auto mod_dir = mods_dir_path() / folder;
    auto moved = engine::move_overwrite_to_mod(overwrite_dir, mod_dir, mods_subpath);
    if (moved) {
        engine::Logger::instance().debug("Moved Overwrite contents to mod: " + folder);
        QMessageBox::information(this, tr("Move content"),
            tr("Overwrite contents moved to mod: %1")
                .arg(QString::fromStdString(folder)));
    } else {
        QMessageBox::warning(this, tr("Move content"), tr("Failed to move Overwrite files."));
    }
}

void MainWindow::sync_overwrite_to_mods() {
    if (current_instance_root_.empty() || !knowledge_) return;
    auto overwrite_dir = overwrite_dir_path();
    auto mods_subpath = knowledge_->get(current_game_id_, "mods_subpath", "");
    if (mods_subpath.empty()) return;
    if (engine::overwrite_is_empty(overwrite_dir)) {
        QMessageBox::information(this, tr("Sync to Mods"), tr("Overwrite folder is empty."));
        return;
    }

    const bool conflict_reversed =
        knowledge_->get(current_game_id_, "conflict_order_reversed", "") == "true";
    const bool include_mod_id =
        knowledge_->get(current_game_id_, "deploy_include_mod_id", "") == "true";
    const auto metadata_file = knowledge_->get(current_game_id_, "metadata_file", "meta.ini");

    // Enabled managed mods only - the conflict engine must see them all
    // (no extension filter, unlike the flags column).
    std::vector<std::pair<std::string, int>> mod_infos;
    for (const auto& m : mod_model_->mods()) {
        if (m.is_separator || m.is_overwrite || m.is_merged || m.is_game_native) continue;
        if (!m.enabled) continue;
        mod_infos.emplace_back(m.id.toStdString(), m.priority);
    }

    // Game-origin destination: a mod folder named after the game.
    const auto game_display =
        current_game_name_.empty() ? current_game_id_ : current_game_name_;
    std::string game_folder = game_display;
    for (char& c : game_folder) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|')
            c = '_';
    }

    SyncOverwriteDialog dialog(SyncOverwriteDialog::Context{
        .overwrite_dir = overwrite_dir,
        .mods_dir = mods_dir_path(),
        .mod_infos = std::move(mod_infos),
        .mods_subpath = mods_subpath,
        .conflict_reversed = conflict_reversed,
        .include_mod_id = include_mod_id,
        .game_dir = current_game_dir_,
        .game_folder = game_folder,
        .game_label = game_display,
        .metadata_file = metadata_file,
    }, this);
    if (dialog.exec() != QDialog::Accepted) return;

    auto targets = dialog.targets();
    if (targets.empty()) return;

    auto moved = engine::apply_sync_plan(targets, overwrite_dir, mods_dir_path(),
                                         mods_subpath, metadata_file, include_mod_id);
    if (moved > 0) {
        engine::Logger::instance().debug("Sync Overwrite: " +
            std::to_string(moved) + " file(s) moved");
        QMessageBox::information(this, tr("Sync to Mods"),
            tr("Moved %1 file(s) from Overwrite to mods.").arg(moved));
    } else {
        QMessageBox::warning(this, tr("Sync to Mods"),
                             tr("Failed to sync Overwrite files."));
    }
}

void MainWindow::open_overwrite_in_file_manager() {
    if (current_instance_root_.empty()) return;
    auto overwrite_dir = overwrite_dir_path();
    std::error_code ec;
    if (!std::filesystem::is_directory(overwrite_dir, ec)) {
        std::filesystem::create_directories(overwrite_dir, ec);
    }
    QDesktopServices::openUrl(
        QUrl::fromLocalFile(QString::fromStdString(overwrite_dir.string())));
}

void MainWindow::show_overwrite_info_dialog() {
    if (current_instance_root_.empty()) return;
    auto overwrite_dir = overwrite_dir_path();
    std::error_code ec;
    if (!std::filesystem::is_directory(overwrite_dir, ec)) {
        std::filesystem::create_directories(overwrite_dir, ec);
    }
    auto mods_subpath = knowledge_ ? knowledge_->get(current_game_id_, "mods_subpath", "") : std::string();

    // Shared modeless dialog - MO2's findChild("__overwriteDialog") pattern.
    auto* dialog = findChild<QDialog*>("__overwriteDialog");
    if (dialog == nullptr) {
        dialog = new ui::OverwriteInfoDialog(overwrite_dir, mods_subpath, this);
        dialog->setObjectName("__overwriteDialog");
        dialog->setAttribute(Qt::WA_DeleteOnClose);
    } else {
        qobject_cast<ui::OverwriteInfoDialog*>(dialog)->set_path(overwrite_dir);
    }
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void MainWindow::move_dropped_overwrite_files(const QStringList& paths,
                                              int mod_row) {
    if (current_instance_root_.empty()) return;
    if (mod_row < 0 || mod_row >= mod_model_->mods().size()) return;
    const auto& target = mod_model_->mods()[mod_row];
    if (target.is_overwrite || target.is_separator || target.is_merged ||
        target.is_game_native) {
        return;
    }
    if (paths.isEmpty()) return;

    auto overwrite_dir = overwrite_dir_path();
    auto mods_subpath = knowledge_ ? knowledge_->get(current_game_id_, "mods_subpath", "") : std::string();
    if (mods_subpath.empty()) return;
    const bool include_mod_id =
        knowledge_ && knowledge_->get(current_game_id_, "deploy_include_mod_id", "") == "true";
    auto mod_dir = mods_dir_path() / target.id.toStdString();
    const auto mod_id = target.id.toStdString();

    bool any = false;
    std::error_code ec;
    const auto ow_canon = std::filesystem::weakly_canonical(overwrite_dir, ec);
    for (const auto& p : paths) {
        const auto canon = std::filesystem::weakly_canonical(p.toStdString(), ec);
        if (ec) {
            ec.clear();
            continue;
        }
        const auto rel = std::filesystem::relative(canon, ow_canon, ec);
        if (ec || rel.empty() || rel == "..") {
            ec.clear();
            continue;
        }
        if (engine::move_overwrite_entry_to_mod(
                overwrite_dir, canon, mod_dir, mods_subpath, include_mod_id, mod_id))
            any = true;
    }

    if (any) {
        engine::Logger::instance().debug(
            "Moved dropped Overwrite entries into mod: " + mod_id);
        recompute_conflicts();
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
    // Removed mods may have been the only owner of plugin files - re-discover.
    request_plugin_refresh();
}

void MainWindow::send_to_separator(const QList<int>& rows) {
    // MO2 sendModsToSeparator (modlistviewactions.cpp:661-729): collect the
    // separators in mod-list order into the shared ListDialog and move the
    // selected mods to the chosen one. Ids ride item data so duplicate display
    // names can't misresolve.
    QStringList names;
    QList<QVariant> ids;
    for (const auto& m : mod_model_->mods()) {
        if (m.is_separator) {
            names << m.name;
            ids << m.id;
        }
    }
    if (names.isEmpty()) return;

    ui::ListDialog dlg(this);
    dlg.setWindowTitle(tr("Select a separator..."));
    dlg.setChoices(names);
    dlg.setChoiceData(ids);
    if (dlg.exec() != QDialog::Accepted) return;
    const QString sep_id = dlg.getChoiceData().toString();
    if (sep_id.isEmpty()) return;

    // Filter to real mod rows (the multi menu skips pinned rows already, but
    // the single-mod path passes one row unconditionally).
    QStringList mod_ids;
    for (int r : rows) {
        if (r < 0 || r >= mod_model_->mods().size()) continue;
        const auto& m = mod_model_->mods()[r];
        if (m.is_separator || m.is_overwrite || m.is_merged || m.is_game_native)
            continue;
        mod_ids << m.id;
    }
    if (mod_ids.isEmpty()) return;

    for (const auto& id : mod_ids)
        mod_model_->set_separator_id(id, sep_id);
    move_mods_to_separator(mod_ids, sep_id);
}

void MainWindow::move_mods_to_separator(const QStringList& ids, const QString& sep_id) {
    // Move every selected mod to the row right below the chosen separator. All
    // ids already carry sep_id (set by send_to_separator); this only relocates
    // rows. Moving top-most subtrees first (descending current row) preserves
    // relative order: each landing at sep_row+1 pushes the previous batch down
    // in the same sequence. Descendants of a selected mod are left to move_mod's
    // block ride-along - moving them explicitly too would lift a child above
    // its own parent.
    const auto& mods = mod_model_->mods();
    int sep_row = -1;
    for (int i = 0; i < mods.size(); ++i) {
        if (mods[i].is_separator && mods[i].id == sep_id) {
            sep_row = i;
            break;
        }
    }
    if (sep_row < 0) return;

    QVector<QPair<int, QString>> rowed;
    // Mirror of ModListModel::is_descendant_of (private there): walk row's
    // parent_id chain to see if it reaches ancestor_id.
    auto is_descendant_of = [&mods](int row, const QString& ancestor_id) {
        QString cur = mods[row].parent_id;
        for (int hops = 0; hops <= mods.size(); ++hops) {
            if (cur == ancestor_id) return true;
            if (cur.isEmpty()) return false;
            int idx = -1;
            for (int i = 0; i < mods.size(); ++i) {
                if (mods[i].id == cur) { idx = i; break; }
            }
            if (idx < 0) return false;
            cur = mods[idx].parent_id;
        }
        return false;
    };
    for (const auto& id : ids) {
        int r = mod_model_->priority_of(id);
        if (r < 0) continue;
        bool covered_by_selected_ancestor = false;
        for (const auto& other : ids) {
            if (other == id) continue;
            int orow = mod_model_->priority_of(other);
            if (orow < 0 || orow >= r) continue;
            if (is_descendant_of(r, other)) {
                covered_by_selected_ancestor = true;
                break;
            }
        }
        if (!covered_by_selected_ancestor)
            rowed << qMakePair(r, id);
    }
    std::sort(rowed.begin(), rowed.end(),
              [](const QPair<int, QString>& a, const QPair<int, QString>& b) {
                  return a.first > b.first;
              });
    for (const auto& [r, id] : rowed)
        mod_model_->move_mod(id, sep_row + 1);
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
        // (below the pinned Overwrite/MERGED which sit at the top).
        mod_model_->move_mod(id, mod_model_->mods().size() - 1);
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
    int target = mod_model_->is_conflict_order_reversed()
        ? mods.size()
        : (ow_row >= 0 ? ow_row : mods.size());

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

        mod_model_->setData(mod_model_->index(r, ModListModel::Name),
            enabled ? Qt::Checked : Qt::Unchecked, Qt::CheckStateRole);
        sync_mod_enable_state(entry.id, enabled);
    }
}

// "Treat mod as root dir" (Tweaks menu): persist the flag in the mod folder's
// meta.ini ([General] rootOverride) and mirror it into the model. When enabled
// the mod deploys to the game root at launch; the Data tab picks it up via
// refresh_data_tab so root files become visible.
void MainWindow::toggle_root_override(const QList<int>& rows, bool on) {
    for (int r : rows) {
        if (r < 0 || r >= mod_model_->mods().size()) continue;
        const auto& entry = mod_model_->mods()[r];
        if (entry.is_separator || entry.is_overwrite || entry.is_merged || entry.is_game_native)
            continue;
        if (entry.root_override == on) continue;

        auto mod_dir = mods_dir_path() / entry.id.toStdString();
        auto meta_ini = mod_dir / "meta.ini";
        std::error_code ec;
        engine::ModMeta meta;
        if (std::filesystem::is_regular_file(meta_ini, ec)) {
            meta = engine::ModMeta::load_file(meta_ini);
        }
        meta.set("General", "rootOverride", on ? "1" : "0");
        if (!meta.save_file(meta_ini)) {
            engine::Logger::instance().warn(
                "toggle_root_override: failed to write " + meta_ini.string());
            continue;
        }
        mod_model_->set_root_override(entry.id, on);
    }
    refresh_data_tab();
}

QString MainWindow::current_nexus_domain() const {
    if (plugin_loader_ && !current_game_id_.empty()) {
        for (const auto& p : plugin_loader_->plugins()) {
            if (p.game_id == current_game_id_)
                return QString::fromStdString(p.nexus_domain);
        }
    }
    return {};
}

SourceVisitInfo MainWindow::source_visit_info(const QString& source_type, const QString& source_id, const QString& page_url) const {
    if (source_type == "steam") {
        return {tr("Visit on Workshop"),
            QString("https://steamcommunity.com/sharedfiles/filedetails/?id=%1").arg(source_id)};
    }
    if (source_type == "nexus") {
        // The game domain comes from the plugin identity (e.g.
        // "skyrimspecialedition") - NEVER the mod id, and there is no
        // "nexus_domain" knowledge hook to read. Empty domain = disabled Visit.
        const QString domain = current_nexus_domain();
        if (domain.isEmpty()) return {tr("Visit on Nexus"), QString()};
        return {tr("Visit on Nexus"),
            QString("https://www.nexusmods.com/%1/mods/%2").arg(domain, source_id)};
    }
    if (source_type == "loverslab") {
        // The stored page URL (the download link minus its ?do=download query)
        // wins - the slug cannot be reconstructed from the file id alone.
        QString url = page_url;
        if (url.isEmpty() && !source_id.isEmpty())
            url = QString("https://www.loverslab.com/files/file/%1/").arg(source_id);
        return {tr("Visit on LoversLab"), url};
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

namespace {

// Write (or, with an empty color, remove) the color key in a separator's
// meta.ini ([General] color) - the same file MO2's ModInfoRegular::setColor
// writes. Returns false only on an actual write failure.
bool write_separator_color_file(const std::filesystem::path& mod_dir, const QString& color) {
    auto meta_path = mod_dir / "meta.ini";
    engine::ModMeta meta;
    if (std::filesystem::exists(meta_path)) {
        std::ifstream f(meta_path);
        if (f) {
            std::string content((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
            meta.parse(content);
        }
    }

    const bool had_color = !meta.get("General", "color").empty();

    if (color.isEmpty()) {
        if (!had_color) return true;  // nothing stored - nothing to clear
        // Rebuild the meta without the color key (ModMeta has no remove).
        engine::ModMeta rebuilt;
        for (const auto& section : meta.sections()) {
            for (const auto& key : meta.keys(section)) {
                if (section == "General" && key == "color") continue;
                rebuilt.set(section, key, meta.get(section, key));
            }
        }
        if (rebuilt.sections().empty()) {
            std::error_code ec;
            std::filesystem::remove(meta_path, ec);
            return true;
        }
        std::ofstream out(meta_path);
        if (!out) return false;
        out << rebuilt.serialize();
        return out.good();
    }

    meta.set("General", "color", color.toStdString());
    std::ofstream out(meta_path);
    if (!out) return false;
    out << meta.serialize();
    return out.good();
}

}  // namespace

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

    // MO2 writes separator metadata into the mod folder's meta.ini; the only
    // persistent field GMM uses is the color. The display name derives from
    // the folder name minus the separator suffix (ModList::getDisplayName),
    // so no explicit name key is needed.
    if (!color.isEmpty()) {
        if (!write_separator_color_file(sep_dir, color)) {
            std::filesystem::remove_all(sep_dir, ec);
            return {};
        }
    }

    // Add to model
    auto id = QString::fromStdString(folder_name);
    mod_model_->add_separator(id, name, color);
    engine::Logger::instance().debug("Separator created: " + name.toStdString());
    return id;
}

void MainWindow::create_separator() {
    if (!knowledge_ || current_game_id_.empty() || current_game_dir_.empty()) return;

    // MO2 createSeparator (modlistviewactions.cpp:152-204): a name-only prompt
    // filtered through fixDirectoryName; the previously used separator color is
    // inherited automatically - there is no color picker in this step.
    QString name;
    while (true) {
        bool ok = false;
        name = QInputDialog::getText(this, tr("Create Separator..."),
            tr("This will create a new separator.\nPlease enter a name:"),
            QLineEdit::Normal, name, &ok);
        if (!ok) return;
        name = QString::fromStdString(engine::sanitize_directory_name(name.toStdString()));
        if (!name.isEmpty()) break;
    }

    // Check for duplicate names
    if (mod_model_->existing_separator_names().contains(name)) {
        QMessageBox::warning(this, tr("Create Separator..."),
            tr("A separator with this name already exists."));
        return;
    }

    auto previous = Settings::instance().previous_separator_color();
    const QString color = previous ? previous->name(QColor::HexArgb) : QString();
    if (create_separator_named(name, color).isEmpty()) {
        QMessageBox::warning(this, tr("Create Separator..."),
            tr("Failed to create separator directory."));
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
    auto dl_dir = downloads_dir_path();
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
                source = source_visit_info(m.source_type, m.source_id,
                                           m.source_page_url).url;
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
                    color.isEmpty() ? QString() : color).isEmpty()) {
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

void MainWindow::open_folder(ui::FolderKind kind) {
    std::filesystem::path target;
    switch (kind) {
    case ui::FolderKind::Game:
        target = current_game_dir_;
        break;
    case ui::FolderKind::MyGames:
    case ui::FolderKind::Inis:
        // INIs live in the profile folder when local INIs are on, else in the
        // game's My Games folder (MO2's openIniFolder semantics).
        if (kind == ui::FolderKind::Inis && Settings::instance().local_inis()) {
            target = profiles_dir_path() / current_profile_name_;
        } else {
            target = game_mygames_dir();
        }
        break;
    case ui::FolderKind::Instance:
        target = current_instance_root_;
        break;
    case ui::FolderKind::Mods:
        target = mods_dir_path();
        break;
    case ui::FolderKind::Profile:
        target = profiles_dir_path() / current_profile_name_;
        break;
    case ui::FolderKind::Downloads:
        target = downloads_dir_path();
        break;
    case ui::FolderKind::Install:
        target = QCoreApplication::applicationDirPath().toStdString();
        break;
    case ui::FolderKind::Plugins:
        target = std::filesystem::path(QCoreApplication::applicationDirPath().toStdString())
            / "plugins";
        break;
    case ui::FolderKind::Themes:
        target = engine::theme_search_dirs(
            QCoreApplication::applicationDirPath().toStdString()).front();
        break;
    case ui::FolderKind::Logs:
        if (platform_) target = platform_->data_dir();
        break;
    }

    if (target.empty()) return;

    std::error_code ec;
    if (!std::filesystem::exists(target, ec))
        std::filesystem::create_directories(target, ec);
    QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(target.string())));
}

void MainWindow::create_separator_at_row(int row) {
    if (!knowledge_ || current_game_id_.empty() || current_game_dir_.empty()) return;

    // Same MO2 flow as create_separator(): name-only prompt, previous color.
    QString name;
    while (true) {
        bool ok = false;
        name = QInputDialog::getText(this, tr("Create Separator..."),
            tr("This will create a new separator.\nPlease enter a name:"),
            QLineEdit::Normal, name, &ok);
        if (!ok) return;
        name = QString::fromStdString(engine::sanitize_directory_name(name.toStdString()));
        if (!name.isEmpty()) break;
    }

    // Check for duplicate names
    if (mod_model_->existing_separator_names().contains(name)) {
        QMessageBox::warning(this, tr("Create Separator..."),
            tr("A separator with this name already exists."));
        return;
    }

    auto previous = Settings::instance().previous_separator_color();
    const QString color = previous ? previous->name(QColor::HexArgb) : QString();
    auto id = create_separator_named(name, color);
    if (id.isEmpty()) {
        QMessageBox::warning(this, tr("Create Separator..."),
            tr("Failed to create separator directory."));
        return;
    }

    // Move the new separator to the target row (below the clicked row)
    int insert_row = row + 1;
    mod_model_->move_mod(id, insert_row);
    engine::Logger::instance().debug("Separator created at row " + std::to_string(insert_row) +
        ": " + name.toStdString());
}

void MainWindow::rename_mod_inline(int row) {
    if (row < 0 || row >= mod_model_->mods().size()) return;
    const auto& mod = mod_model_->mods()[row];
    if (mod.is_overwrite || mod.is_merged || mod.is_game_native) return;
    mod_view_->edit(mod_model_->index(row, ModListModel::Name));
}

void MainWindow::apply_rename(int row, const QString& name) {
    const auto revert = [this, row]() {
        emit mod_model_->dataChanged(mod_model_->index(row, ModListModel::Name),
                                     mod_model_->index(row, ModListModel::Version));
    };

    if (row < 0 || row >= mod_model_->mods().size()) return;
    const auto& entry = mod_model_->mods()[row];
    if (entry.is_overwrite || entry.is_merged || entry.is_game_native) { revert(); return; }

    if (name == entry.name) { revert(); return; }  // unchanged

    if (!knowledge_ || current_game_id_.empty() || current_game_dir_.empty()) { revert(); return; }

    auto mods_subpath = knowledge_->get(current_game_id_, "mods_subpath", "");
    auto separator_suffix = knowledge_->get(current_game_id_, "separator_suffix", "_separator");
    if (mods_subpath.empty()) { revert(); return; }

    // Internal name = folder name on disk (MO2 makeInternalName): separators
    // get the suffix appended, everything else is the raw name.
    auto clean = engine::sanitize_directory_name(name.toStdString());
    if (clean.empty()) {
        QMessageBox::warning(this, tr("Rename"), tr("Invalid name."));
        revert();
        return;
    }
    const QString display_name = QString::fromStdString(clean);
    std::string internal = clean;
    if (entry.is_separator) internal += separator_suffix;
    const QString new_id = QString::fromStdString(internal);

    if (new_id == entry.id) { revert(); return; }  // sanitized back to the same folder

    // Duplicate check (case-insensitive, excluding self) - MO2 renameMod.
    for (const auto& m : mod_model_->mods()) {
        if (m.id == entry.id) continue;
        if (m.id.compare(new_id, Qt::CaseInsensitive) == 0) {
            QMessageBox::warning(this, tr("Rename"),
                tr("Name is already in use by another mod."));
            revert();
            return;
        }
    }

    auto mods_dir = mods_dir_path();
    const auto old_path = mods_dir / entry.id.toStdString();
    const auto new_path = mods_dir / new_id.toStdString();

    std::error_code ec;
    if (std::filesystem::exists(new_path, ec)) {
        QMessageBox::warning(this, tr("Rename"),
            tr("A folder named %1 already exists in the mods directory.").arg(new_id));
        revert();
        return;
    }

    if (std::filesystem::exists(old_path, ec)) {
        std::filesystem::rename(old_path, new_path, ec);
        if (ec) {
            QMessageBox::warning(this, tr("Rename"), tr("Failed to rename mod folder."));
            revert();
            return;
        }
    }

    // Move the instance-meta sidecar along with the folder so source/separator
    // info isn't lost; keep its [GameModManager] folder key in sync.
    auto meta_dir = meta_dir_path();
    if (!meta_dir.empty()) {
        auto old_meta = meta_dir / (entry.id.toStdString() + ".ini");
        auto new_meta = meta_dir / (new_id.toStdString() + ".ini");
        if (std::filesystem::exists(old_meta, ec)) {
            std::filesystem::rename(old_meta, new_meta, ec);
            if (!ec) {
                auto meta = engine::ModMeta::load(meta_dir, new_id.toStdString());
                meta.set("GameModManager", "folder", new_id.toStdString());
                meta.save(meta_dir, new_id.toStdString());
            }
        }
    }

    mod_model_->rename_mod_in_place(row, new_id, display_name);
    // Renamed folder -> owner_mod attribution in the Plugins tab must follow.
    request_plugin_refresh();
    engine::Logger::instance().debug("Renamed mod: " + entry.name.toStdString() +
        " -> " + display_name.toStdString());
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

void MainWindow::select_color_for_selected() {
    auto sel = mod_view_->selectionModel()->selectedRows();
    if (sel.isEmpty()) return;

    const auto& ref = mod_model_->mods()[sel.first().row()];
    QColor current;
    if (ref.is_separator && !ref.separator_color.isEmpty())
        current = QColor(ref.separator_color);

    // MO2 setColor (modlistviewactions.cpp:1195-1224): standalone color dialog
    // with alpha; prefills the current color or the remembered previous one.
    QColorDialog dialog(this);
    dialog.setOption(QColorDialog::ShowAlphaChannel);
    if (current.isValid()) {
        dialog.setCurrentColor(current);
    } else if (auto prev = Settings::instance().previous_separator_color()) {
        dialog.setCurrentColor(*prev);
    }
    if (dialog.exec() != QDialog::Accepted) return;

    const auto color = dialog.currentColor();
    if (!color.isValid()) return;

    Settings::instance().set_previous_separator_color(color);
    const QString hex = color.name(QColor::HexArgb);

    for (const auto& idx : sel) {
        int row = idx.row();
        if (row < 0 || row >= mod_model_->mods().size()) continue;
        const auto& mod = mod_model_->mods()[row];
        if (!mod.is_separator) continue;
        write_separator_color_file(mods_dir_path() / mod.id.toStdString(), hex);
        mod_model_->set_mod_color(mod.id, color);
    }
}

void MainWindow::reset_color_for_selected() {
    auto sel = mod_view_->selectionModel()->selectedRows();
    if (sel.isEmpty()) return;

    for (const auto& idx : sel) {
        int row = idx.row();
        if (row < 0 || row >= mod_model_->mods().size()) continue;
        const auto& mod = mod_model_->mods()[row];
        if (!mod.is_separator) continue;
        write_separator_color_file(mods_dir_path() / mod.id.toStdString(), QString());
        mod_model_->clear_mod_color(mod.id);
    }
    Settings::instance().remove_previous_separator_color();
}

void MainWindow::request_save_order() {
    if (loading_) return;
    if (save_order_timer_->isActive()) save_order_timer_->stop();
    save_order_timer_->start();
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

    // Remove old mod_order / folded_separators / folded_mods / mod_parents /
    // toolbar_shortcuts / toolbar_shortcut_icons lines
    std::istringstream stream(existing);
    std::string line;
    std::string cleaned;
    while (std::getline(stream, line)) {
        auto key_pos = line.find("mod_order");
        if (key_pos != std::string::npos) continue;
        auto fold_pos = line.find("folded_separators");
        if (fold_pos != std::string::npos) continue;
        auto fm_pos = line.find("folded_mods");
        if (fm_pos != std::string::npos) continue;
        auto np_pos = line.find("mod_parents");
        if (np_pos != std::string::npos) continue;
        auto ts_pos = line.find("toolbar_shortcuts");
        if (ts_pos != std::string::npos) continue;
        auto tsi_pos = line.find("toolbar_shortcut_icons");
        if (tsi_pos != std::string::npos) continue;
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

    // Folded mods (visual nesting; name-keyed like folded_separators, so a
    // renamed mod loses its fold state - consistent with the separator rule).
    cleaned += "folded_mods = [";
    bool first_fm = true;
    for (const auto& m : mods) {
        if (!m.is_separator && m.folded) {
            if (!first_fm) cleaned += ", ";
            cleaned += "\"" + m.name.toStdString() + "\"";
            first_fm = false;
        }
    }
    cleaned += "]\n";

    // Visual-nesting parent links: "child_id=parent_id". Id-based, so renames
    // survive (rename_mod_in_place cascades parent_id on the model).
    cleaned += "mod_parents = [";
    bool first_mp = true;
    for (const auto& m : mods) {
        if (m.parent_id.isEmpty()) continue;
        if (!first_mp) cleaned += ", ";
        cleaned += "\"" + m.id.toStdString() + "=" + m.parent_id.toStdString() + "\"";
        first_mp = false;
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

    // Per-shortcut custom icon paths (parallel to toolbar_shortcuts, same
    // index). "-" marks "no custom icon" (the naive TOML parser below drops
    // empty tokens, which would desync the arrays); restore maps it back to
    // empty and falls back to exe extraction.
    cleaned += "toolbar_shortcut_icons = [";
    bool first_tsi = true;
    for (const auto& icon : toolbar_shortcut_icons_) {
        if (!first_tsi) cleaned += ", ";
        cleaned += "\"" + (icon.isEmpty() ? "-" : icon.toStdString()) + "\"";
        first_tsi = false;
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
    std::vector<std::string> folded_mod_names;  // visual nesting (folded mods)
    std::vector<std::pair<std::string, std::string>> parent_links;  // "child" -> "parent"
    std::vector<std::string> toolbar_paths;
    std::vector<std::string> toolbar_icons;

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

        auto fm_pos = line.find("folded_mods");
        if (fm_pos != std::string::npos) {
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
                        folded_mod_names.push_back(token.substr(s, e - s + 1));
                    }
                }
            }
            continue;
        }

        auto np_pos = line.find("mod_parents");
        if (np_pos != std::string::npos) {
            auto bracket = line.find('[');
            auto close_bracket = line.find(']');
            if (bracket != std::string::npos && close_bracket != std::string::npos) {
                auto content = line.substr(bracket + 1, close_bracket - bracket - 1);
                std::istringstream ss(content);
                std::string token;
                while (std::getline(ss, token, ',')) {
                    auto s = token.find_first_not_of(" \t\"");
                    auto e = token.find_last_not_of(" \t\"");
                    if (s == std::string::npos || e == std::string::npos) continue;
                    auto entry = token.substr(s, e - s + 1);
                    auto eq = entry.find('=');
                    if (eq == std::string::npos || eq == 0 || eq == entry.size() - 1) continue;
                    parent_links.emplace_back(entry.substr(0, eq), entry.substr(eq + 1));
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

        auto tsi_pos = line.find("toolbar_shortcut_icons");
        if (tsi_pos != std::string::npos) {
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
                        toolbar_icons.push_back(token.substr(s, e - s + 1));
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
    auto apply_fold = [&folded_names, &folded_mod_names](ModEntry& m) {
        // Separators use folded_separators, folded mods use folded_mods (both
        // name-keyed). Either way the flag is reset first so a stale entry in
        // the persisted list can't resurrect a fold that was later unfolded.
        const auto& names = m.is_separator ? folded_names : folded_mod_names;
        m.folded = false;
        for (const auto& fn : names) {
            if (m.name.toStdString() == fn) { m.folded = true; break; }
        }
    };

    // Id-based nesting links from instance.toml's mod_parents.
    QHash<QString, QString> parent_map;
    for (const auto& [child, parent] : parent_links) {
        parent_map[QString::fromStdString(child)] = QString::fromStdString(parent);
    }

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
        // Game-native (unmanaged) mods own the top band: hoist them above the
        // user entries, preserving relative order on both sides.
        std::stable_partition(reordered.begin(), reordered.end(),
            [](const ModEntry& m) { return m.is_game_native; });
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
            // Game-native (unmanaged) mods form a fixed top band - they can
            // never sort below user mods, whatever priority got persisted. A
            // separator placed above the band (lower priority than the
            // natives) keeps its place so its fold can hide the native mods.
            if (a.is_game_native != b.is_game_native) {
                if (a.is_separator) return key(a) < key(b);
                if (b.is_separator) return key(b) > key(a);
                return a.is_game_native;
            }
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

    // Restore persisted visual-nesting parent links and re-validate them
    // (sanitize_parent_links clears dangling / kind-mismatched / cyclic links,
    // e.g. a child whose parent was deleted or renamed out of existence).
    mod_model_->restore_parent_links(parent_map);

    // Ensure apply_fold_state() reflects current flags
    mod_model_->apply_fold_state();

    // Restore toolbar shortcuts
    engine::Logger::instance().begin_group(engine::LogLevel::Debug, "Restored toolbar shortcuts");
    for (size_t i = 0; i < toolbar_paths.size(); ++i) {
        // "-" is the persisted empty-icon sentinel (see save_order)
        auto icon = (i < toolbar_icons.size() && toolbar_icons[i] != "-")
            ? QString::fromStdString(toolbar_icons[i])
            : QString();
        add_toolbar_shortcut_from_path(QString::fromStdString(toolbar_paths[i]), icon);
    }
    engine::Logger::instance().end_group();

    loading_ = false;

    sync_separator_ids();
}

// Find the closing ']' of a TOML array whose opening '[' is at position
// `from`. Brackets inside double-quoted strings (JSON escapes included) and
// nested arrays (e.g. the "env":[] of an executable entry) are skipped so a
// ']' inside an entry never truncates the section early.
static size_t find_toml_array_end(const std::string& s, size_t from) {
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (size_t i = from; i < s.size(); ++i) {
        const char c = s[i];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '[') {
            ++depth;
        } else if (c == ']') {
            if (depth == 0) return i;
            --depth;
        }
    }
    return std::string::npos;
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

    // Remove the ENTIRE previous `executables = [...]` block (header through
    // its closing ']' line). Stripping only the header line would leave the
    // entries and ']' behind as orphaned garbage that cascades on every save.
    std::string cleaned = existing;
    auto header = cleaned.find("executables = [");
    if (header != std::string::npos) {
        auto open = header + std::string("executables = [").size();
        auto close = find_toml_array_end(cleaned, open);
        if (close != std::string::npos) {
            auto line_end = cleaned.find('\n', close);
            auto block_end = (line_end == std::string::npos) ? cleaned.size() : line_end + 1;
            cleaned.erase(header, block_end - header);
        }
    }
    while (!cleaned.empty() && (cleaned.back() == '\n' || cleaned.back() == '\r'))
        cleaned.pop_back();

    // Collect JSON objects from the combo
    auto entries = right_panel_->exec_controls()->executable_entries();
    QStringList json_entries;
    for (const auto& e : entries) {
        auto raw = QString::fromUtf8(QJsonDocument(e.toJson()).toJson(QJsonDocument::Compact));
        json_entries.append(raw);
    }

    cleaned += "\nexecutables = [\n";
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
    auto end = find_toml_array_end(content, start);
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

    auto icon_cache = cache_thumbnails_dir_path();
    // Restore the last selected executable for this instance. On a fresh
    // instance the selection is empty - just populate the list and let the
    // user pick. Staging is passed so merged-view (mod-provided) executables
    // still get icons after a deploy.
    auto staging = current_instance_root_.empty()
        ? std::filesystem::path()
        : current_instance_root_ / ".gmm_staging";
    right_panel_->exec_controls()->set_executables(
        exec_list, pending_exec_selection_, current_game_dir_, icon_cache, staging);

    // Persist immediately on first run so future launches use the saved list
    if (saved_executables_.empty())
        save_executables();
}

void MainWindow::launch_game() {
    // Re-entry guard: a fast double-click / Enter on the focused Run button can
    // fire run_clicked twice while the first launch is still in flight. The
    // overlay covers the mouse but not the keyboard, so guard explicitly.
    if (running_process_pid_ > 0) {
        engine::Logger::instance().debug(
            "Launch skipped - game already running (pid " +
            std::to_string(running_process_pid_) + ")");
        return;
    }

    auto entry = right_panel_->exec_controls()->current_entry();
    if (entry.path.isEmpty() || entry.path == kAddNewEntryText) {
        QMessageBox::warning(this, tr("Launch"), tr("No executable selected."));
        return;
    }
    if (current_game_dir_.empty()) {
        QMessageBox::warning(this, tr("Launch"), tr("Game directory not set."));
        return;
    }

    // Resolve against the canonical game dir spelling so the path matches the
    // overlay mountpoint (game_dir commonly goes through ~/.steam ->
    // ~/.local/share/Steam). Entry paths are merged-view (deploy-relative);
    // the namespace-local overlay makes them reachable at launch even though
    // they may not exist physically here. Reachability is validated after
    // deploy in launch_with_executable - entries are never auto-removed.
    std::error_code ce;
    auto canon_game = std::filesystem::weakly_canonical(current_game_dir_, ce);
    auto exec_path = (ce || canon_game.empty() ? current_game_dir_ : canon_game) /
                     entry.path.toStdString();

    // Output-to-mod routing: resolve the target mod folder, auto-creating it
    // (with the game's metadata file) when it doesn't exist yet.
    const auto output_mod_dir = ensure_output_mod_dir(entry.output_mod);
    launch_with_executable(QString::fromStdString(exec_path.string()), output_mod_dir);
}

std::filesystem::path MainWindow::ensure_output_mod_dir(const QString& mod_name) {
    if (mod_name.isEmpty())
        return {};
    auto output_mod_dir = mods_dir_path() / mod_name.toStdString();
    std::error_code ec;
    if (!std::filesystem::is_directory(output_mod_dir, ec)) {
        std::filesystem::create_directories(output_mod_dir, ec);
        if (ec) {
            engine::Logger::instance().error(
                "Failed to create output mod folder " +
                output_mod_dir.string() + ": " + ec.message());
            return {};
        }
        auto metadata_file = knowledge_
            ? knowledge_->get(current_game_id_, "metadata_file", "meta.ini")
            : "meta.ini";
        engine::ModMeta::write_game_metadata(output_mod_dir, metadata_file,
            mod_name.toStdString(), "1.0", "0");
        engine::Logger::instance().debug(
            "Output-to-mod: created mod folder " + output_mod_dir.string());
    }
    return output_mod_dir;
}

static void gmm_debug(const char* fmt, ...) {
    static bool enabled = gmm_debug_enabled();
    if (!enabled) return;
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[GMM] ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}

void MainWindow::launch_with_executable(const QString& full_path,
                                        const std::filesystem::path& output_mod_dir) {
    // Re-entry guard (also covers toolbar shortcuts, which call this directly).
    if (running_process_pid_ > 0) {
        engine::Logger::instance().debug(
            "Launch skipped - game already running (pid " +
            std::to_string(running_process_pid_) + ")");
        return;
    }
    // A previous launch's deploy is still running (P8.4: the deploy now runs
    // on the worker thread). The continuation will launch or clean up.
    if (launch_prep_pending_) {
        engine::Logger::instance().debug(
            "Launch skipped - deploy already in progress");
        return;
    }

    auto& trace = engine::TraceRecorder::instance();
    trace.begin_flow("launch");

    auto exec_path = std::filesystem::path(full_path.toStdString());

    // Output-to-mod routing (MO2 getByBinary parity): an explicit target from
    // the exec-controls combo wins; otherwise the launched binary's configured
    // output mod is resolved from the executable entries, so toolbar shortcuts
    // and Data-tab Execute honor it too. Unmatched binaries fall back to
    // Overwrite capture.
    std::filesystem::path effective_output = output_mod_dir;
    if (effective_output.empty() && !current_game_dir_.empty()) {
        const QString mod_name = ui::output_mod_for_path(
            right_panel_->exec_controls()->executable_entries(),
            current_game_dir_, full_path);
        if (!mod_name.isEmpty())
            effective_output = ensure_output_mod_dir(mod_name);
    }
    if (!effective_output.empty()) {
        const QString folder =
            QString::fromStdString(effective_output.filename().string());
        bool disabled = false;
        for (const auto& m : mod_model_->mods()) {
            if (m.id.compare(folder, Qt::CaseInsensitive) == 0) {
                disabled = !m.enabled;
                break;
            }
        }
        if (disabled) {
            engine::Logger::instance().error(
                "Launch blocked - output mod '" + folder.toStdString() + "' is disabled");
            QMessageBox::warning(this, tr("Launch"),
                tr("The designated write target \"%1\" is not enabled.\n\n"
                   "Enable the mod and try again.").arg(folder));
            trace.end_flow("launch", false, "Output mod disabled");
            return;
        }
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

    // Build a launch-prep snapshot and run the deploy on the worker thread
    // (P8.4). engine::prepare_launch_params deploys all enabled mods into
    // .gmm_staging and returns the assembled params; the worker emits
    // prepared() only after that finished, so launch_game() (in
    // on_launch_params_prepared) provably never starts before the staging
    // tree is complete.
    trace.begin_stage("launch", "Prepare launch environment");
    engine::LaunchPrepRequest req;
    req.instance_root = current_instance_root_;
    req.game_dir = current_game_dir_;
    req.executable = exec_path;
    req.knowledge = knowledge_ ? *knowledge_ : engine::GameKnowledge();
    req.game_id = current_game_id_;
    req.steam_appid = steam_appid;
    req.is_windows_exe = (exec_path.extension().string() == ".exe" ||
                          exec_path.extension().string() == ".EXE");
    req.local_saves_enabled = Settings::instance().local_saves();
    req.platform = platform_;

    // Per-executable environment overrides, resolved the same way output-to-mod
    // routing is (first matching executable entry owns the launch): Run button,
    // toolbar shortcuts and Data-tab Execute all inherit the configured env.
    const QStringList env_list = ui::environment_for_path(
        right_panel_->exec_controls()->executable_entries(),
        current_game_dir_, full_path);
    for (const auto& v : env_list)
        req.environment.push_back(v.toStdString());
    if (!req.environment.empty()) {
        engine::Logger::instance().debug(
            "Launch env: " + std::to_string(req.environment.size()) +
            " override(s) from executable entry for " + full_path.toStdString());
    }

    if (!launch_deploy_thread_) {
        launch_deploy_thread_ = new ui::DeployThread(this);
        connect(launch_deploy_thread_->worker(), &ui::DeployWorker::progress,
                this, &MainWindow::on_deploy_progress);
        connect(launch_deploy_thread_->worker(), &ui::DeployWorker::prepared,
                this, &MainWindow::on_launch_params_prepared);
    }

    launch_prep_pending_ = true;
    output_mod_dir_ = effective_output;
    output_session_scratch_.clear();
    launch_deploy_thread_->start(std::move(req));
    // Returns immediately; the launch continues in on_launch_params_prepared.
}

void MainWindow::on_deploy_progress(int files_done, int files_total) {
    if (!launch_prep_pending_) return;
    if (files_total > 0) {
        game_lock_label_->setText(tr("Deploying mods… %1/%2")
            .arg(files_done).arg(files_total));
    }
}

void MainWindow::on_launch_params_prepared(engine::LaunchParams lparams) {
    if (!launch_prep_pending_) return;
    launch_prep_pending_ = false;

    auto& trace = engine::TraceRecorder::instance();
    auto exec_path = lparams.executable;
    auto binary_name = QString::fromStdString(exec_path.filename().string());

    // The user may have switched instances while the deploy ran: the staging
    // tree belongs to the OLD instance. Drop the stale result - never launch
    // into the wrong game.
    if (lparams.game_dir != current_game_dir_) {
        engine::Logger::instance().warn(
            "Launch abandoned - instance changed while mods were deploying");
        trace.end_flow("launch", false, "Instance changed mid-deploy");
        hide_game_lock_overlay();
        return;
    }
    trace.end_stage("launch", true, "Launch environment prepared");

    lparams.platform = platform_;
    lparams.overwrite_dir = overwrite_dir_path();

    // MO2-equivalent plugin order: build + write the game's Plugins.txt (and
    // the instance profile) right before launch. No-op for games without
    // plugin support (no localappdata_folder hook).
    trace.begin_stage("launch", "Sync plugin order");
    engine::PluginDatabase::write_plugins_txt_for_launch(
        current_game_dir_, current_instance_root_, current_game_id_,
        lparams.steam_appid, knowledge_ ? *knowledge_ : engine::GameKnowledge(), platform_);
    trace.end_stage("launch", true, "Plugin order synced");

    // Output-to-mod: capture into a per-launch scratch dir, relay on exit.
    // Empty output_mod_dir_ = default Overwrite capture (toolbar shortcuts).
    output_session_scratch_.clear();
    if (!output_mod_dir_.empty() && lparams.use_overlay) {
        auto scratch_base = cache_dir_path();
        if (scratch_base.empty()) scratch_base = current_game_dir_ / "cache";
        auto session = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        output_session_scratch_ = scratch_base / "exec-output" /
            ("sess-" + std::to_string(session));
        std::error_code ec;
        std::filesystem::create_directories(output_session_scratch_, ec);
        if (ec) {
            engine::Logger::instance().error(
                "Failed to create output scratch dir: " + ec.message());
            output_session_scratch_.clear();
            output_mod_dir_.clear();
        } else {
            lparams.output_capture_dir = output_session_scratch_;
            engine::Logger::instance().debug(
                "Output-to-mod: capturing to " + output_session_scratch_.string());
        }
    } else if (!output_mod_dir_.empty()) {
        // Direct-symlink mode captures nothing: game writes land in game_dir,
        // so there is no session to relay into the output mod.
        engine::Logger::instance().warn(
            "Output-to-mod unavailable: game launches in direct-symlink mode "
            "(writes go to game_dir)");
        output_mod_dir_.clear();
    }

    if (!lparams.extra_lowerdirs.empty())
        staging_dir_ = lparams.extra_lowerdirs.back();
    overlay_session_ = lparams.use_overlay;

    // Merged-view existence check, AFTER deploy so staging is populated: the
    // file may be game-native (physical), live-overlay (mounted), or a
    // deployed mod file under .gmm_staging. Entries are kept either way - a
    // "missing" file usually just means its mod is disabled.
    if (!engine::merged_view_file_exists(current_game_dir_, staging_dir_, exec_path)) {
        trace.end_flow("launch", false, "Executable not found");
        hide_game_lock_overlay();
        if (!output_session_scratch_.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(output_session_scratch_, ec);
            output_session_scratch_.clear();
        }
        output_mod_dir_.clear();
        QMessageBox::warning(this, tr("Launch"),
            tr("The selected executable is not reachable in the game directory.\n%1\n\n"
               "If it belongs to a mod, make sure that mod is enabled.")
                .arg(QString::fromStdString(exec_path.string())));
        return;
    }
    trace.end_stage("launch", true, "Overlay/staging paths ready");

    trace.begin_stage("launch", "Launch executable");
    auto lresult = engine::launch_game(lparams);

    if (lresult.pid <= 0) {
        trace.end_stage("launch", false, "launch_game returned no PID");
        hide_game_lock_overlay();
        QMessageBox::warning(this, tr("Launch"), tr("Failed to launch game."));
        trace.end_flow("launch", false, "Failed to launch game");
        if (!output_session_scratch_.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(output_session_scratch_, ec);
            output_session_scratch_.clear();
        }
        output_mod_dir_.clear();
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

    // P1.3 event bus: mirror MO2 onAboutToRun — emitted only once the launch
    // actually succeeded (a PID exists) so a failed launch is not reported.
    engine::EventBus::instance().dispatch(
        engine::events::kGameLaunched,
        engine::json_obj({
            {"exe", exec_path.string()},
            {"args", ""},
        }));

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
        engine::EventBus::instance().dispatch(
            engine::events::kGameFinished,
            engine::json_obj({{"exit_code", "0"}}));
        if (!staging_dir_.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(staging_dir_, ec);
            staging_dir_.clear();
        }
        // Delay capture so any child/spawned processes finish writing
        auto t = launch_time_;
        QTimer::singleShot(Settings::instance().overlay_capture_delay_ms(),
                           this, [this, t]() { do_capture_overwrite(t); });
    }
#else
    // ---- cgroup v2 path (primary) ----
    if (!cgroup_path_.empty()) {
        if ((process_tree_checkbox_ && process_tree_checkbox_->isChecked())
            && !engine::cgroup_is_empty({cgroup_path_}))
            refresh_process_tree();

        if (engine::cgroup_is_empty({cgroup_path_})) {
            // A Steam handoff reparents the game's processes to the subreaper
            // supervisor, which stays alive until its last child exits.  The
            // cgroup can be empty in that window while the game still runs.
            // Don't declare the game exited while the supervisor is waiting on
            // reparented children - that would hide the lock overlay and clear
            // the launch guard mid-session.
            //
            // But early-reap the supervisor first: once its last child is
            // reaped it _exit(0)s, and a zombie supervisor still answers
            // kill(pid,0)==0 - which would misclassify a cleanly-exited game
            // as "reparented" forever, leaving the lock overlay up with zero
            // processes.  Same rule as the PGID zombie-gate war story:
            // waitpid(WNOHANG) before trusting kill() liveness.
            bool supervisor_gone = false;
            if (running_process_pid_ > 0) {
                int st;
                pid_t r = waitpid(static_cast<pid_t>(running_process_pid_), &st, WNOHANG);
                supervisor_gone = (r == static_cast<pid_t>(running_process_pid_)) ||
                                  (r < 0 && errno == ECHILD);
            }
            if (!supervisor_gone && running_process_pid_ > 0 &&
                (kill(static_cast<pid_t>(running_process_pid_), 0) == 0 ||
                 errno == EPERM)) {
                engine::Logger::instance().debug(
                    "Watchdog: cgroup empty but supervisor alive (reparented game) - continuing");
                if (process_tree_checkbox_ && process_tree_checkbox_->isChecked())
                    refresh_process_tree();
                return;
            }
            engine::Logger::instance().debug(
                "Watchdog: cgroup empty, game fully exited");
            int supervisor_exit = reap_supervisor(static_cast<pid_t>(running_process_pid_));
            flush_pending_changes();
            hide_game_lock_overlay();
            trace.end_stage("launch", true, "Game exited");
            trace.end_flow("launch", true, "Game session finished");
            engine::cgroup_remove({cgroup_path_});
            running_process_pid_ = -1;
            cgroup_path_.clear();
            if (process_watch_timer_) process_watch_timer_->stop();
            // P1.3 event bus: mirror MO2 onFinishedRun.
            engine::EventBus::instance().dispatch(
                engine::events::kGameFinished,
                engine::json_obj(
                    {{"exit_code", std::to_string(supervisor_exit)}}));
            if (!staging_dir_.empty()) {
                std::error_code ec;
                std::filesystem::remove_all(staging_dir_, ec);
                staging_dir_.clear();
            }
            auto t = launch_time_;
            QTimer::singleShot(Settings::instance().overlay_capture_delay_ms(),
                               this, [this, t]() { do_capture_overwrite(t); });
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
    // P1.3 event bus: mirror MO2 onFinishedRun (fallback PGID path).
    engine::EventBus::instance().dispatch(
        engine::events::kGameFinished,
        engine::json_obj({{"exit_code",
                           WIFEXITED(reap_status) ? std::to_string(WEXITSTATUS(reap_status))
                                                  : "-1"}}));
    if (!staging_dir_.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(staging_dir_, ec);
        staging_dir_.clear();
    }
    auto t = launch_time_;
    QTimer::singleShot(Settings::instance().overlay_capture_delay_ms(),
                       this, [this, t]() { do_capture_overwrite(t); });
#endif
}

void MainWindow::apply_mod_filter() {
    if (!mod_model_ || !mod_view_) return;

    // Start from a clean fold state
    mod_model_->apply_fold_state();

    const QString text = filter_bar_->filter_text().trimmed().toLower();
    const QString group = filter_bar_->current_group();
    const auto& mods = mod_model_->mods();

    // Fold-hidden set (pure model computation): a folded separator band scope
    // or a folded mod subtree. Filtered-out rows inside a fold scope must stay
    // hidden and must never be re-shown by the ancestor propagation below.
    QVector<bool> fold_hidden(mods.size(), false);
    for (int row = 0; row < mods.size(); ++row)
        fold_hidden[row] = mod_model_->is_row_fold_hidden(row);

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
        else if (group == "FOMOD")
            group_match = m.is_fomod;
        else if (group == "Separators")
            group_match = false;  // regular mods hidden when viewing separators only

        visible[row] = text_match && group_match;

        // If an active fold scope (folded separator band or folded mod subtree)
        // hides this row, hide it too - fold overrides search.
        if (visible[row] && group == "All" && fold_hidden[row]) {
            visible[row] = false;
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

        // A separator inside a folded scope (a folded parent's band or a
        // folded mod subtree) must STAY hidden: apply_fold_state() hid it and
        // the force-show branches above would otherwise re-show it (the bug
        // was nested separators staying visible under a folded parent).
        // Mirrors the fold-overrides-search rule applied to mod rows (which
        // only overrides when group == "All").
        if (visible[row] && group == "All" && fold_hidden[row]) {
            visible[row] = false;
        }
        mod_view_->setRowHidden(row, QModelIndex(), !visible[row]);
    }

    // Nesting: a filtered-out ancestor (mod parent or separator) stays visible
    // while any of its subtree members matches, so the tree never breaks
    // mid-level under a filter. Fold-hidden rows are never re-shown (the fold
    // override above already hid their visible members, so they can't re-show
    // via a descendant either - the check keeps it airtight).
    if (mod_model_->nesting_enabled()) {
        bool changed = true;
        while (changed) {
            changed = false;
            for (int row = 0; row < mods.size(); ++row) {
                if (visible[row] || fold_hidden[row]) continue;
                if (mod_model_->has_visible_descendant(row, visible)) {
                    visible[row] = true;
                    changed = true;
                }
            }
        }
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

    // Direct-symlink session: no overlay, no capture - the game wrote straight
    // into game_dir and the files must stay there. Nothing to harvest, and
    // capture_overwrite would MOVE the game's files out of game_dir.
    if (!overlay_session_) {
        output_session_scratch_.clear();
        output_mod_dir_.clear();
        return;
    }

    bool case_insensitive =
        knowledge_ && knowledge_->get(current_game_id_, "case_sensitive", "true") == "false";

    bool session_active = !output_session_scratch_.empty() && !output_mod_dir_.empty();
    auto capture_dir = session_active ? output_session_scratch_
                                      : overwrite_dir_path();

    // When launched via overlay, all writes already went directly into the
    // capture dir (upperdir = session scratch for output-mod, Overwrite otherwise).
    if (overlay_launched_) {
        overlay_launched_ = false;
        engine::Logger::instance().debug(
            "Overlay launched: writes already in " + capture_dir.string());
    } else {
        engine::capture_overwrite(current_game_dir_, capture_dir, capture_time,
                                  case_insensitive);
    }

    // Fold CI-duplicate directories (Meshes/ + meshes/ split by the game's raw
    // case-insensitive writes) back together, for both capture paths. The
    // capture already normalizes internally; this covers the overlay upperdir
    // and the output-session scratch (so relayed mods are merged too).
    if (case_insensitive && engine::normalize_overwrite_casing(capture_dir) > 0) {
        engine::Logger::instance().debug(
            "Overwrite: merged case-insensitive directory duplicates in " +
            capture_dir.string());
    }

    if (session_active) {
        auto mods_subpath = knowledge_
            ? knowledge_->get(current_game_id_, "mods_subpath", "") : std::string();
        auto inc_id = knowledge_->get(current_game_id_, "deploy_include_mod_id", "false");
        auto overwrite_dir = overwrite_dir_path();
        auto relayed = engine::relay_output_to_mod(output_session_scratch_,
            output_mod_dir_, overwrite_dir, mods_subpath,
            inc_id == "true", output_mod_dir_.filename().string());
        engine::Logger::instance().debug(
            "Output-to-mod: relayed " + std::to_string(relayed) +
            " file(s) to " + output_mod_dir_.string() +
            " (P2: the mod is the full write target, Overwrite untouched)");
        std::error_code ec;
        std::filesystem::remove_all(output_session_scratch_, ec);
        output_session_scratch_.clear();
        output_mod_dir_.clear();
    }

    // Reload mod list so the mod / Overwrite contents become visible
    std::error_code ec;
    if (session_active || std::filesystem::exists(capture_dir, ec))
        load_mods_from_game();
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

    // Same merged-view resolution as launch_game: canonical base + the entry's
    // deploy-relative path. Reachability is validated at launch (after deploy);
    // entries that only exist in the merged view are valid toolbar targets.
    std::error_code ce;
    auto canon_game = std::filesystem::weakly_canonical(current_game_dir_, ce);
    auto exec_path = (ce || canon_game.empty() ? current_game_dir_ : canon_game) /
                     entry.path.toStdString();

    auto exec_path_qstr = QString::fromStdString(exec_path.string());
    add_toolbar_shortcut_from_path(exec_path_qstr, entry.icon_path);
}

void MainWindow::add_toolbar_shortcut_from_path(const QString& full_path,
                                                  const QString& icon_path) {
    if (toolbar_shortcut_paths_.contains(full_path)) return;

    QIcon icon;
    if (!icon_path.isEmpty()) {
        QPixmap pix(icon_path);
        if (!pix.isNull())
            icon = QIcon(pix);
    }
    if (icon.isNull())
        icon = ui::extractExeIcon(full_path, cache_thumbnails_dir_path());

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
    toolbar_shortcut_icons_.append(icon_path);
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

    // Same merged-view resolution as the toolbar: canonical game dir spelling
    // + the entry's deploy-relative path. A .desktop file runs OUTSIDE the
    // launch namespace, so only physically present executables can be a
    // desktop target - mod-provided ones get an honest message instead.
    std::error_code ce;
    auto canon_game = std::filesystem::weakly_canonical(current_game_dir_, ce);
    auto exec_path = (ce || canon_game.empty() ? current_game_dir_ : canon_game) /
                     entry.path.toStdString();
    if (!std::filesystem::exists(exec_path)) {
        QMessageBox::warning(this, tr("Shortcut"),
            tr("Executable not found:\n%1\n\n"
               "Mod-provided executables can only be launched from within "
               "GameModManager.")
                .arg(QString::fromStdString(exec_path.string())));
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

    auto icon_cache = cache_thumbnails_dir_path();

    // Entries are never auto-pruned here: with merged-view (deploy-relative)
    // paths a "missing" file usually just means its mod is disabled, and it
    // must not be deleted on that basis. A "Clean entries" sweep (MO2-style)
    // is planned separately.
    auto existing = right_panel_->exec_controls()->executable_entries();

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
    // Stretch 1 + the addStretch(1) spacers above/below: the tree fills a
    // proportional share of the overlay height and shrinks as the window does,
    // so the Copy/Unlock/Kill controls below it always stay in view.
    layout->addWidget(process_tree_, 1);
    
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
            engine::cgroup_remove({cgroup_path_});
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
    // Grab keyboard focus so Space/Enter can't re-activate the still-focused
    // Run button while the game is starting. The overlay covers the mouse but
    // not the keyboard without this.
    game_lock_overlay_->setFocus();
    game_lock_overlay_->grabKeyboard();
    if (pid > 0 && process_tree_checkbox_ && process_tree_checkbox_->isChecked())
        refresh_process_tree();
}

void MainWindow::hide_game_lock_overlay() {
    locked_pid_ = -1;
    if (game_lock_overlay_ && game_lock_overlay_->isVisible())
        game_lock_overlay_->releaseKeyboard();
    game_lock_overlay_->hide();
}

void MainWindow::set_ui_enabled(bool enabled) {
    // Lock or unlock the whole manager surface (mod list, panels, console,
    // menus, toolbars). The install dialogs (FOMOD wizard, name confirm,
    // overwrite query, progress popup) are top-level children of `this`, NOT
    // of the disabled content widgets, so they stay interactive while the
    // manager itself is greyed out - the same shape MO2's UILocker produces.
    if (centralWidget()) centralWidget()->setEnabled(enabled);
    if (menu_bar_) menu_bar_->setEnabled(enabled);
    if (toolbar_area_) toolbar_area_->setEnabled(enabled);
    if (profile_bar_) profile_bar_->setEnabled(enabled);
    if (status_bar_) status_bar_->setEnabled(enabled);
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
        // P1.3 event bus: mirror MO2 onModStateChanged for the deferred
        // (game-running) toggle path — the state only actually changed on disk
        // here, so this is the moment to emit, not at queue time.
        engine::EventBus::instance().dispatch(
            engine::events::kModStateChanged,
            engine::json_obj({
                {"mod", pt.mod_id.toStdString()},
                {"enabled", pt.enabled ? "1" : "0"},
            }));
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
    // Closing cancels in-flight downloads. Warn first unless the user told us
    // to never ask again ("Don't Ask" persists the preference). MO2 asks the
    // same question (mainwindow.cpp canExit); here Ok/Quit/Don't Ask all
    // proceed with the close, Cancel aborts it.
    auto* dt = right_panel_ ? right_panel_->downloads_tab() : nullptr;
    if (dt && dt->has_active_download() &&
        Settings::instance().confirm_close_with_downloads()) {
        QMessageBox box(this);
        box.setWindowTitle(tr("Active Downloads"));
        box.setIcon(QMessageBox::Warning);
        box.setText(tr("You have active downloads in progress.\n"
                       "Closing the application will cancel them."));
        auto* ok_btn = box.addButton(tr("Ok"), QMessageBox::AcceptRole);
        auto* quit_btn = box.addButton(tr("Quit"), QMessageBox::AcceptRole);
        auto* dont_ask_btn = box.addButton(tr("Don't Ask"), QMessageBox::AcceptRole);
        auto* cancel_btn = box.addButton(tr("Cancel"), QMessageBox::RejectRole);
        box.setDefaultButton(cancel_btn);
        box.exec();

        QAbstractButton* clicked = box.clickedButton();
        if (clicked == cancel_btn) {
            event->ignore();
            return;
        }
        if (clicked == dont_ask_btn) {
            Settings::instance().set_confirm_close_with_downloads(false);
        }
        // Ok / Quit / Don't Ask all fall through to the close.
    }

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
    (void)header_state;  // mod-list header layout is NOT restored from disk:
                         // a state saved with a different column count shifts
                         // the old layout (Stretch/Interactive) onto the wrong
                         // columns. The header always uses the setup in
                         // setup_mod_view() instead.

    if (!geo.isEmpty()) pending_geometry_ = geo;
    if (!win_state.isEmpty()) restoreState(win_state);
    if (main_splitter_ && !main_split.isEmpty()) main_splitter_->restoreState(main_split);
    if (console_splitter_ && !console_split.isEmpty()) console_splitter_->restoreState(console_split);

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
    return downloads_dir_path() / ".download_manifest.json";
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
    auto downloads_dir = downloads_dir_path();
    dt->deserialize(json, downloads_dir);
}

void MainWindow::wire_downloads_tab() {
    auto* dt = right_panel_->downloads_tab();
    if (!dt) return;

    // Manifest first, then the dir scan: deserialize populates pipeline
    // entries (ids like "<mod_id>-<file_id>") before the scan, so their
    // archives are recognized as already-tracked instead of duplicated as
    // "Manual" rows.
    load_download_manifest();
    dt->set_downloads_dir(downloads_dir_path());

    connect(dt, &DownloadsTab::install_requested,
            this, [this](const std::string& mod_id, const std::filesystem::path& fp,
                         const std::string& source_type, const std::string& source_id,
                         int file_id, const std::string& display_name,
                         const std::string& page_url) {
        if (!pipeline_thread_) return;
        // Lock the interface for the duration of the install so the user can't
        // race it (re-trigger, edit the mod list, quit mid-copy). Released by
        // install_complete / install_canceled.
        set_ui_enabled(false);
        QMetaObject::invokeMethod(pipeline_thread_->worker(),
            [this, mod_id, fp, source_type, source_id, file_id, display_name, page_url]() {
            pipeline_thread_->worker()->install_mod(
                mod_id, fp.string(), source_type, source_id, file_id, display_name,
                page_url);
        }, Qt::QueuedConnection);
    });
    connect(dt, &DownloadsTab::loverslab_url_entered,
            this, &MainWindow::start_loverslab_download);
    connect(dt, &DownloadsTab::pause_requested,
            this, [this](const std::string& id) {
        if (!pipeline_thread_) return;
        QMetaObject::invokeMethod(pipeline_thread_->worker(), [this, id]() {
            pipeline_thread_->worker()->pause_download(id);
        }, Qt::QueuedConnection);
    });
    connect(dt, &DownloadsTab::resume_requested,
            this, [this](const std::string& id) {
        if (!pipeline_thread_) return;
        auto* dtab = right_panel_->downloads_tab();
        if (dtab) dtab->mark_downloading(id);
        auto mods_dir = mods_dir_path().string();
        auto meta_dir = current_instance_root_.empty()
            ? "" : (current_instance_root_ / "meta").string();
        // Nexus downloads resume with their original NXM link...
        auto it_nxm = nxm_links_.find(id);
        if (it_nxm != nxm_links_.end()) {
            auto link = it_nxm->second;
            QMetaObject::invokeMethod(pipeline_thread_->worker(),
                [this, id, link, mods_dir, meta_dir]() {
                pipeline_thread_->worker()->download_mod(
                    id, link, current_game_id_, mods_dir, meta_dir);
            }, Qt::QueuedConnection);
            return;
        }
        // ...LoversLab downloads resume with their original URL.
        auto it_url = url_downloads_.find(id);
        if (it_url != url_downloads_.end()) {
            auto url = it_url->second;
            QMetaObject::invokeMethod(pipeline_thread_->worker(),
                [this, id, url, mods_dir, meta_dir]() {
                pipeline_thread_->worker()->download_mod_url(
                    id, url, current_game_id_, mods_dir, meta_dir);
            }, Qt::QueuedConnection);
        }
    });
    connect(dt, &DownloadsTab::entry_removed,
            this, [this](const std::string& id) {
            nxm_links_.erase(id);
            url_downloads_.erase(id);
            save_download_manifest();
        });
}

void MainWindow::wire_saves_tab() {
    auto* st = right_panel_->saves_tab();
    if (!st) return;

    // Saves live under documents/My Games/<game> (game_mygames_dir()). The
    // "Saves" leaf is knowledge-driven where the game plugin defines it; the
    // Bethesda-family default applies to Skyrim/FO4/etc.
    auto sub = knowledge_->get(current_game_id_, "saves_subpath", "Saves");
    const auto saves_dir = game_mygames_dir() / sub;
    engine::Logger::instance().debug("Saves tab: scanning " + saves_dir.string());
    st->set_saves_dir(saves_dir);

    // No directory watcher and no mod-list/plugin-driven rescans: the Saves
    // dir is scanned exactly once here (at load, when the tab is wired) and
    // after a delete. Earlier versions re-scanned on every mod_list_changed
    // (via refresh_plugins_tab) to keep the missing-asset column in sync, but
    // that made a separator fold/unfold trigger a full save scan - so the
    // missing-asset column now reflects launch-time load order only.
    connect(st, &ui::SavesTab::delete_requested,
            this, &MainWindow::on_saves_delete_requested);

    // Initial fill (the delete flow triggers the only later scan).
    on_saves_refresh_requested();
}

void MainWindow::on_saves_refresh_requested() {
    auto* st = right_panel_->saves_tab();
    if (!st) return;

    ui::SavesScanRequest request;
    request.saves_dir = st->saves_dir();
    if (request.saves_dir.empty()) return;
    request.extensions = {"ess"};
    request.game_id = current_game_id_;
    // Snapshot the plugin list so results reflect the load order at the moment
    // the refresh was asked for (missing-asset state moves with toggles).
    request.plugins = plugins_db_.plugins();
    request.mods_dir = mods_dir_path();
    request.overwrite_dir = overwrite_dir_path();
    st->request_scan(std::move(request));
}

void MainWindow::on_saves_delete_requested(const QStringList& filepaths) {
    // Trash (never permanent): engine::remove_path -> QDir::moveToTrash.
    for (const auto& fp : filepaths) {
        engine::remove_path(std::filesystem::path(fp.toStdString()), /*permanent=*/false);
    }
    // The saved state no longer matches disk; re-scan.
    on_saves_refresh_requested();
}

void MainWindow::update_install_progress(const std::string& mod_id, int percent,
                                         const std::string& status) {
    // A new install resets the popup (title, bar) and (re)arms the deferred
    // show. Subsequent updates for the same install just refresh it.
    if (mod_id != active_install_progress_id_) {
        active_install_progress_id_ = mod_id;
        if (!install_progress_dialog_) {
            install_progress_dialog_ = new ui::InstallProgressDialog(this);
        }
        install_progress_dialog_->begin(tr("Installing…"));
        if (!install_progress_show_timer_) {
            install_progress_show_timer_ = new QTimer(this);
            install_progress_show_timer_->setSingleShot(true);
            connect(install_progress_show_timer_, &QTimer::timeout,
                    this, [this]() {
                if (install_progress_dialog_) install_progress_dialog_->show();
            });
        }
        // ~300ms delay so a quick install never flashes the dialog (MO2
        // behaves the same way - the popup only appears for the slow part).
        install_progress_show_timer_->start(300);
    } else if (install_progress_dialog_ && !install_progress_dialog_->isVisible()) {
        // The dialog was hidden by an interactive install dialog (FOMOD wizard,
        // name confirm, overwrite) - it is back to being informative now, so
        // show it immediately (the 300ms delay already elapsed long ago).
        install_progress_dialog_->show();
    }

    if (install_progress_dialog_) {
        install_progress_dialog_->set_status(QString::fromStdString(status), percent);
    }
}

void MainWindow::hide_install_progress() {
    active_install_progress_id_.clear();
    if (install_progress_show_timer_) install_progress_show_timer_->stop();
    if (install_progress_dialog_) install_progress_dialog_->hide();
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

void MainWindow::start_loverslab_download(const std::string& url) {
    if (!engine::LoversLabProvider::is_loverslab_url(url)) {
        QMessageBox::warning(this, tr("Download from URL"),
            tr("Not a LoversLab download link.\n\n"
               "Right-click a file's Download button on loverslab.com and copy "
               "the link address (it ends in ?do=download), then paste it here."));
        return;
    }

    if (!engine::LoversLabAuth::instance().has_cookie()) {
        QMessageBox::information(this, tr("Download from URL"),
            tr("No LoversLab session cookie is configured.\n\n"
               "LoversLab has no public API, so downloads need the session "
               "cookie from a signed-in browser tab. Set it under Settings > "
               "Sources > LoversLab, then try again."));
        return;
    }

    // Redact the CSRF token in logs - it is a session-bound secret.
    std::string log_url = url;
    {
        auto kp = log_url.find("csrfKey=");
        if (kp != std::string::npos) {
            auto ke = log_url.find_first_of("&", kp);
            log_url = log_url.substr(0, kp + 8) +
                      (ke != std::string::npos ? log_url.substr(ke) : "");
        }
    }
    engine::Logger::instance().debug("LoversLab download: " + log_url);

    // Entry key is the file id when the URL carries one, else a stable hash
    // (keeps map keys and archive-name fallbacks free of '/' characters).
    std::string file_id = engine::LoversLabProvider::extract_file_id(url);
    const std::string key = file_id.empty()
        ? "ll-" + std::to_string(std::hash<std::string>{}(url))
        : file_id;

    auto* dt = right_panel_->downloads_tab();
    if (dt) {
        dt->add_download(
            key,
            tr("LoversLab file %1").arg(QString::fromStdString(key)).toStdString(),
            "LoversLab", {}, {}, 0, {},
            engine::LoversLabProvider::mod_page_url(url));
    }

    // Surface the download: bring the window to front and switch to the
    // Downloads tab so the user sees the new entry start.
    if (isMinimized()) {
        showNormal();
    }
    raise();
    activateWindow();
    right_panel_->show_downloads_tab();

    // Keep the URL so a paused download can be resumed later.
    url_downloads_[key] = url;

    // Build paths for the pipeline context
    auto mods_dir = mods_dir_path();
    auto meta_dir = current_instance_root_.empty()
        ? "" : (current_instance_root_ / "meta").string();

    QMetaObject::invokeMethod(pipeline_thread_->worker(),
        [this, key, url, mods_dir, meta_dir]() {
        pipeline_thread_->worker()->download_mod_url(
            key, url, current_game_id_, mods_dir.string(), meta_dir);
    }, Qt::QueuedConnection);

    engine::Logger::instance().debug("LoversLab download queued: " + key);
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

    // Find the nexus_domain for this game from the loaded plugin identity
    const QString nexus_domain = current_nexus_domain();

    // No nexus_domain means this game doesn't support Nexus Mods at all
    if (nexus_domain.isEmpty()) return;

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
            engine::GameSource{"nexus", "nexusmods.com", nexus_domain.toStdString()});

        engine::Logger::instance().debug(
            "Registered " + current_game_id_ + " for Nexus Mods downloads (nexusmods.com)");
    }
}

void MainWindow::ensure_nxm_handler_default() {
    if (nxm_handler_check_done_) return;
    nxm_handler_check_done_ = true;

#ifdef GMM_PLATFORM_LINUX
    // Respect a permanent "don't ask again" choice
    if (Settings::instance().nxm_handler_check() == "dont_ask") return;

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
        Settings::instance().set_nxm_handler_check("dont_ask");
        engine::Logger::instance().debug("nxm:// handler check suppressed (don't show again)");
    } else {
        engine::Logger::instance().debug("nxm:// handler check declined; will ask next launch");
    }
#else
    (void)0;
#endif
}

void MainWindow::show_proton_panel() {
    if (current_instance_root_.empty()) {
        QMessageBox::information(this, tr("Proton Options"),
                                 tr("No instance is currently loaded."));
        return;
    }

    engine::Instance inst = engine::Instance::from_root(current_instance_root_);
    inst.read_toml();

    uint32_t steam_appid = inst.info().steam_appid;
    if (steam_appid == 0 && knowledge_) {
        auto id_str = knowledge_->get(current_game_id_, "steam_appid", "");
        if (!id_str.empty()) {
            try { steam_appid = std::stoul(id_str); } catch (...) {}
        }
    }
    const std::string game_name = current_game_name_.empty()
        ? current_game_id_
        : current_game_name_;

    // Gather the deploy config through the same engine function the launch
    // path uses, so the panel's actions behave exactly like the launch-time
    // deploy. The effective strategy (instance.toml override, else knowledge)
    // seeds the panel's "Deployment strategy" selector and decides whether the
    // direct-deploy actions are usable.
    engine::GameKnowledge empty_knowledge;
    const engine::GameKnowledge& knowledge = knowledge_ ? *knowledge_ : empty_knowledge;
    const engine::DeployConfig deploy_cfg = engine::deploy_config_for(
        current_instance_root_, current_game_dir_, knowledge, current_game_id_);
    const std::string effective_strategy =
        engine::effective_deploy_strategy(current_instance_root_, knowledge,
                                          current_game_id_);

    ui::ProtonPanel dlg(platform_, plugin_loader_, current_game_id_, game_name,
                        current_game_dir_, steam_appid, current_instance_root_,
                        inst.info().proton_runner, effective_strategy,
                        deploy_cfg, this);
    if (dlg.exec() == QDialog::Accepted) {
        auto runner = dlg.selected_runner();
        engine::Instance write = engine::Instance::from_root(current_instance_root_);
        write.read_toml();
        write.write_key("proton_runner", runner);
        current_instance_ = write;
    }
}

engine::ProtonToolRequest MainWindow::current_proton_request() const {
    engine::ProtonToolRequest request;
    if (current_instance_root_.empty()) return request;
    request.platform = platform_;
    request.game_dir = current_game_dir_;

    engine::Instance inst = engine::Instance::from_root(current_instance_root_);
    if (inst.read_toml()) {
        request.runner_override = inst.info().proton_runner;
        request.steam_appid = inst.info().steam_appid;
    }
    if (request.steam_appid == 0 && knowledge_) {
        auto id_str = knowledge_->get(current_game_id_, "steam_appid", "");
        if (!id_str.empty()) {
            try { request.steam_appid = std::stoul(id_str); } catch (...) {}
        }
    }
    return request;
}

void MainWindow::run_prefix_tool(const QStringList& args) {
    auto request = current_proton_request();
    if (request.platform == nullptr || request.steam_appid == 0) {
        QMessageBox::information(this, tr("Proton Tools"),
            tr("No Steam game is loaded — a Proton prefix is required."));
        return;
    }

    std::vector<std::string> argv;
    for (const auto& a : args) argv.push_back(a.toStdString());

    int64_t pid = engine::run_proton_tool(request, argv);
    if (pid < 0) {
        QMessageBox::warning(this, tr("Proton Tools"),
            tr("No protontricks / winetricks / Wine available to run this tool."));
    }
}

void MainWindow::run_exe_in_prefix() {
    auto request = current_proton_request();
    if (request.platform == nullptr || request.steam_appid == 0) {
        QMessageBox::information(this, tr("Proton Tools"),
            tr("No Steam game is loaded — a Proton prefix is required."));
        return;
    }

    const QString file = QFileDialog::getOpenFileName(
        this, tr("Run an .exe in this prefix"),
        QString::fromStdString(current_game_dir_.string()),
        tr("Windows executables (*.exe);;All files (*)"));
    if (file.isEmpty()) return;

    int64_t pid = engine::run_proton_exe(request, std::filesystem::path(file.toStdString()));
    if (pid < 0) {
        QMessageBox::warning(this, tr("Proton Tools"),
            tr("Failed to run:\n%1").arg(file));
    }
}

void MainWindow::show_settings_dialog() {
    SettingsDialog dlg(style_manager_, native_style_name_, current_instance_root_,
                       plugin_loader_, this);
    dlg.exec();
    // Per-folder path overrides may have changed in the dialog.
    if (!current_instance_root_.empty()) {
        current_instance_ = engine::Instance::from_root(current_instance_root_);
        current_instance_.read_toml();
    }
    // The icon-pack (and theme) settings may have changed in the dialog:
    // re-sync IconManager and re-apply the persistent window/toolbar icons.
    // Context menus build their icons on demand, so they pick it up already.
    {
        auto& icon_mgr = engine::IconManager::instance();
        icon_mgr.set_current_theme(Settings::instance().theme().toStdString());
        icon_mgr.set_mode(Settings::instance().icon_pack().toStdString());
        toolbar_->reapply_icons();
        qApp->setWindowIcon(icon_mgr.resolve_icon("gmm-logo"));
    }
    // The separator-scrollbar setting may have changed in the dialog.
    if (mod_view_) mod_view_->apply_scrollbar_policy();
    // The per-instance nesting setting may have changed in the dialog.
    apply_nesting_setting();
    // The compact-downloads setting may have changed in the dialog.
    if (auto* dt = right_panel_->downloads_tab()) dt->apply_compact_style();
    // The Nexus queue-downloads setting may have changed in the dialog: push
    // the new value into the fetch pool.
    pipeline_thread_->worker()->set_nexus_queue_downloads(
        Settings::instance().nexus_queue_downloads());
}

void MainWindow::apply_nesting_setting() {
    if (current_instance_root_.empty()) return;
    const auto key = QString::fromStdString(current_instance_root_.filename().string());
    mod_model_->set_nesting_enabled(Settings::instance().modlist_nested(key));
}

void MainWindow::show_instance_statistics() {
    if (current_instance_root_.empty()) {
        QMessageBox::information(this, tr("Instance Statistics"),
                                 tr("No instance is currently loaded."));
        return;
    }

    auto cache_dir = cache_dir_path();
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
            for (const auto& p : plugin_loader_->game_plugins()) {
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
            for (const auto& p : plugin_loader_->game_plugins()) {
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
