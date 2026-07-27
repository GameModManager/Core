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
#include "ui/pipeline_worker.h"
#include "engine/log/logger.h"
#include "engine/detect/mod_scanner.h"
#include "engine/registry/game_knowledge.h"
#include "engine/instance/instance.h"
#include "engine/pipeline/sync_stage.h"
#include "runtime/runtime.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
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
#include <QSplitter>
#include <QStatusBar>
#include <QStyle>
#include <QTemporaryDir>
#include <QTextStream>
#include <QToolBar>
#include <QUrl>
#include <QVBoxLayout>

#include <fstream>
#include <memory>
#include <sstream>

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
    toolbar_area_->addWidget(toolbar_);
    addToolBar(toolbar_area_);

    // QToolBar tells us when orientation changes (horizontal ↔ vertical)
    connect(toolbar_area_, &QToolBar::orientationChanged, this, [this](Qt::Orientation orient) {
        toolbar_->set_vertical(orient == Qt::Vertical);
    });

    connect(toolbar_, &MainToolbar::settings_clicked, this, [this]() {
        QMessageBox::information(this, "Settings", "Settings - coming soon");
    });
    connect(toolbar_, &MainToolbar::instances_clicked, this, [this]() {
        QMessageBox::information(this, "Instances", "Switch Instance - coming soon");
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

    // Set alternating row colors via palette
    auto pal = mod_view_->palette();
    pal.setColor(QPalette::Base, pal.color(QPalette::Base));
    pal.setColor(QPalette::AlternateBase, pal.color(QPalette::Base).lighter(108));
    mod_view_->setPalette(pal);

    // Sync checkbox toggles to filesystem (disable.it)
    connect(mod_model_, &QAbstractItemModel::dataChanged,
            this, [this](const QModelIndex& topLeft, const QModelIndex& bottomRight, const QVector<int>& roles) {
        (void)bottomRight;
        if (roles.contains(Qt::CheckStateRole) && topLeft.column() == ModListModel::Enabled) {
            auto id = mod_model_->data(topLeft.sibling(topLeft.row(), ModListModel::Name), Qt::EditRole).toString();
            bool enabled = mod_model_->data(topLeft, Qt::CheckStateRole).toInt() == Qt::Checked;
            sync_mod_enable_state(id, enabled);
        }
    });

    // Sync priority rewrites to metadata files after reorder
    connect(mod_model_, &ModListModel::mod_list_changed, this, &MainWindow::sync_priorities);

    // Save order on every model change
    connect(mod_model_, &ModListModel::mod_list_changed, this, [this]() {
        if (!loading_) save_order();
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

    // --- Status bar ---
    status_bar_ = new GmmStatusBar(this);
    statusBar()->addWidget(status_bar_, 1);

    engine::Logger::instance().info("UI initialized");
    engine::Logger::instance().debug("Console panel ready");

    pipeline_thread_ = new PipelineThread(this);
    pipeline_thread_->start();

    // Restore saved app state (window geometry, splitters, column sizes)
    restore_app_state();

    connect(right_panel_->exec_controls(), &ExecControlsBar::run_clicked, this, &MainWindow::launch_game);

    connect(right_panel_->exec_controls(), &ExecControlsBar::shortcut_to_toolbar,
            this, &MainWindow::add_shortcut_to_toolbar);

    connect(right_panel_->exec_controls(), &ExecControlsBar::shortcut_to_desktop,
            this, &MainWindow::add_shortcut_to_desktop);

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
    current_game_id_ = game_id;
    current_game_name_ = game_display_name;
    current_profile_name_ = profile_name;
    current_game_dir_ = game_dir;
    current_instance_root_ = instance_root;
    update_title();

    if (!game_dir.empty() && knowledge_) {
        load_mods_from_game();
        populate_executables();
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

void MainWindow::setup_menu_bar() {
    menu_bar_ = new AppMenuBar(this);
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
        engine::Logger::instance().info("Opening recent instance: " + name.toStdString());
    });
    connect(menu_bar_, &AppMenuBar::import_mods_requested, this, [this]() {
        QMessageBox::information(this, "Import Mods", "Import Mods - coming soon");
    });
    connect(menu_bar_, &AppMenuBar::export_mods_requested, this, [this]() {
        QMessageBox::information(this, "Export Mods", "Export Mods - coming soon");
    });
    connect(menu_bar_, &AppMenuBar::settings_requested, this, [this]() {
        QMessageBox::information(this, "Settings", "Settings - coming soon");
    });
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
        engine::Logger::instance().info("Enabled " + std::to_string(sel.size()) + " mods");
    });
    connect(menu_bar_, &AppMenuBar::disable_selected_requested, this, [this]() {
        auto sel = mod_view_->selectionModel()->selectedRows();
        for (const auto& idx : sel) {
            mod_model_->setData(idx, Qt::Unchecked, Qt::CheckStateRole);
        }
        engine::Logger::instance().info("Disabled " + std::to_string(sel.size()) + " mods");
    });
    connect(menu_bar_, &AppMenuBar::priority_up_requested, this, [this]() {
        engine::Logger::instance().info("Priority Up requested");
    });
    connect(menu_bar_, &AppMenuBar::priority_down_requested, this, [this]() {
        engine::Logger::instance().info("Priority Down requested");
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
        engine::Logger::instance().info("Refresh requested");
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
        engine::Logger::instance().info("Running tool: " + tool_id.toStdString() +
                                        " for game: " + game_id.toStdString());
    });
    connect(menu_bar_, &AppMenuBar::open_instance_folder_requested, this, [this]() {
        engine::Logger::instance().info("Open instance folder");
    });
    connect(menu_bar_, &AppMenuBar::open_mods_folder_requested, this, [this]() {
        engine::Logger::instance().info("Open mods folder");
    });
    connect(menu_bar_, &AppMenuBar::open_downloads_folder_requested, this, [this]() {
        engine::Logger::instance().info("Open downloads folder");
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
}

void MainWindow::sync_mod_enable_state(const QString& mod_id, bool enabled) {
    if (loading_) return;
    if (!knowledge_ || current_game_id_.empty() || current_game_dir_.empty()) return;

    // Separators don't have enable/disable on disk
    for (const auto& m : mod_model_->mods()) {
        if (m.id == mod_id && m.is_separator) return;
    }

    auto mods_subpath = knowledge_->get(current_game_id_, "mods_subpath", "");
    if (mods_subpath.empty()) return;

    auto mod_folder = current_game_dir_ / mods_subpath / mod_id.toStdString();

    if (enabled) {
        (void)engine::ModScanner::enable_mod(*knowledge_, current_game_id_, mod_folder);
    } else {
        (void)engine::ModScanner::disable_mod(*knowledge_, current_game_id_, mod_folder);
    }
}

void MainWindow::sync_priorities() {
    if (loading_) return;
    if (!knowledge_ || current_game_id_.empty() || current_game_dir_.empty()) return;

    auto mods_subpath = knowledge_->get(current_game_id_, "mods_subpath", "");
    if (mods_subpath.empty()) return;

    auto& mods = mod_model_->mods();
    for (int i = 0; i < mods.size(); ++i) {
        if (mods[i].is_overwrite || mods[i].is_separator || mods[i].id == kOverwriteModId) continue;
        auto mod_folder = current_game_dir_ / mods_subpath / mods[i].id.toStdString();
        (void)engine::ModScanner::set_priority(*knowledge_, current_game_id_, mod_folder, i);
    }
}

void MainWindow::load_mods_from_game() {
    if (!knowledge_ || current_game_id_.empty() || current_game_dir_.empty()) return;

    loading_ = true;

    // Configure conflict order from plugin hook (before adding mods)
    auto conflict_reversed = knowledge_->get(current_game_id_, "conflict_order_reversed", "");
    mod_model_->set_conflict_order_reversed(conflict_reversed == "true");

    auto scanned = engine::ModScanner::scan(*knowledge_, current_game_id_, current_game_dir_);

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
            mod_model_->add_mod(id, name, ver, mod.priority);
            if (!mod.enabled) {
                mod_model_->toggle_mod(id);
            }
        }
    }

    loading_ = false;

    // Restore saved order (including separators)
    load_order();

    // Symlink Overwrite into the game's mods directory
    auto mods_subpath = knowledge_->get(current_game_id_, "mods_subpath", "");
    if (!mods_subpath.empty() && !current_instance_root_.empty()) {
        auto game_mods_dir = current_game_dir_ / mods_subpath;
        auto overwrite_dir = current_instance_root_ / "mods" / "Overwrite";
        (void)engine::ModScanner::symlink_overwrite(game_mods_dir, overwrite_dir);
    }

    engine::Logger::instance().info("Loaded " + std::to_string(scanned.size()) +
        " mods for " + current_game_name_);
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

    connect(mod_view_, &QWidget::customContextMenuRequested,
            this, [this, clear_action, create_mod_action, edit_separator_action, delete_separator_action, remove_action](const QPoint& pos) {
        auto idx = mod_view_->indexAt(pos);
        if (!idx.isValid()) return;

        int row = idx.row();
        bool is_ow = mod_model_->is_overwrite(row);
        bool is_sep = row >= 0 && row < mod_model_->mods().size() && mod_model_->mods()[row].is_separator;

        clear_action->setVisible(is_ow);
        create_mod_action->setVisible(is_ow);
        edit_separator_action->setVisible(is_sep);
        delete_separator_action->setVisible(is_sep);
        remove_action->setVisible(!is_ow && !is_sep);

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
            auto overwrite_dir = current_instance_root_ / "mods" / "Overwrite";
            if (engine::SyncStage::clear_overwrite(overwrite_dir)) {
                engine::Logger::instance().info("Overwrite cleared");
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

        auto overwrite_dir = current_instance_root_ / "mods" / "Overwrite";
        auto mods_subpath = knowledge_ ? knowledge_->get(current_game_id_, "mods_subpath", "") : std::string();
        if (mods_subpath.empty()) return;

        auto mod_dir = current_game_dir_ / mods_subpath / name.toStdString();

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
            engine::Logger::instance().info("Promote Overwrite to mod: " + name.toStdString());
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
                auto mod_folder = current_game_dir_ / mods_subpath / entry.id.toStdString();
                std::error_code ec;
                std::filesystem::remove_all(mod_folder, ec);
                if (ec) {
                    engine::Logger::instance().error("Failed to remove mod folder: " + mod_folder.string() + ": " + ec.message());
                }
            }
            mod_model_->remove_mod(entry.id);
        }
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
    auto sep_dir = current_game_dir_ / mods_subpath / folder_name;

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
    engine::Logger::instance().info("Separator created: " + name.toStdString());
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

    auto mods_dir = current_game_dir_ / mods_subpath;
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

    engine::Logger::instance().info("Separator edited: " + new_name.toStdString());
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
        auto sep_folder = current_game_dir_ / mods_subpath / mod.id.toStdString();
        std::error_code ec;
        std::filesystem::remove_all(sep_folder, ec);
        if (ec) {
            engine::Logger::instance().error("Failed to remove separator folder: " + sep_folder.string());
        }
    }

    mod_model_->remove_mod(mod.id);
    engine::Logger::instance().info("Separator deleted: " + mod.name.toStdString());
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

    // Remove old mod_order lines
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

    // Append mod_order
    cleaned += "mod_order = [";
    auto& mods = mod_model_->mods();
    for (int i = 0; i < mods.size(); ++i) {
        if (mods[i].is_overwrite) continue;
        cleaned += "\"" + mods[i].id.toStdString() + "\"";
        if (i < mods.size() - 1 && !mods[i + 1].is_overwrite) cleaned += ", ";
    }
    cleaned += "]\n";

    // Append folded separators
    cleaned += "folded_separators = [";
    bool first = true;
    for (const auto& m : mods) {
        if (m.is_separator && m.folded) {
            if (!first) cleaned += ", ";
            cleaned += "\"" + m.name.toStdString() + "\"";
            first = false;
        }
    }
    cleaned += "]\n";

    // Append toolbar shortcuts
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
    std::vector<std::string> order;
    std::vector<std::string> folded_names;
    std::vector<std::string> toolbar_paths;

    while (std::getline(in, line)) {
        auto key_pos = line.find("mod_order");
        if (key_pos != std::string::npos) {
            // Parse: mod_order = ["folder1", "folder2", ...]
            auto bracket = line.find('[');
            auto close_bracket = line.find(']');
            if (bracket != std::string::npos && close_bracket != std::string::npos) {
                auto content = line.substr(bracket + 1, close_bracket - bracket - 1);
                std::istringstream ss(content);
                std::string token;
                while (std::getline(ss, token, ',')) {
                    // trim whitespace and quotes
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

    if (order.empty() && toolbar_paths.empty()) return;

    // Reorder the model based on saved order
    loading_ = true;

    auto& mods = mod_model_->mods();
    QVector<ModEntry> reordered;

    // Extract Overwrite entry (must stay at position 0)
    ModEntry overwrite_entry;
    bool has_overwrite = false;
    QMap<QString, int> id_to_idx;
    for (int i = 0; i < mods.size(); ++i) {
        if (mods[i].is_overwrite) {
            overwrite_entry = mods[i];
            has_overwrite = true;
        } else {
            id_to_idx[mods[i].id] = i;
        }
    }

    // Place Overwrite first if present
    if (has_overwrite) {
        reordered.append(overwrite_entry);
    }

    // Place entries in saved order
    for (const auto& folder_str : order) {
        auto folder_id = QString::fromStdString(folder_str);
        if (id_to_idx.contains(folder_id)) {
            reordered.append(mods[id_to_idx[folder_id]]);
            id_to_idx.remove(folder_id);
        }
    }

    // Append any remaining entries not in the saved order (new mods/separators)
    for (int i = 0; i < mods.size(); ++i) {
        if (id_to_idx.contains(mods[i].id)) {
            reordered.append(mods[i]);
        }
    }

    // Apply fold state before resetting
    for (auto& m : reordered) {
        if (m.is_separator) {
            m.folded = false;
            for (const auto& fn : folded_names) {
                if (m.name.toStdString() == fn) {
                    m.folded = true;
                    break;
                }
            }
        }
    }

    loading_ = false;
    mod_model_->reset_with_order(reordered);
    loading_ = true;

    mod_model_->apply_fold_state();

    // Restore toolbar shortcuts
    for (const auto& path : toolbar_paths) {
        add_toolbar_shortcut_from_path(QString::fromStdString(path));
    }

    engine::Logger::instance().info("Loaded saved order (" + std::to_string(order.size()) + " entries)");
}

void MainWindow::populate_executables() {
    if (!knowledge_ || current_game_id_.empty()) return;

    auto execs_csv = knowledge_->get(current_game_id_, "executables", "");
    auto default_exec = knowledge_->get(current_game_id_, "default_executable", "");

    if (execs_csv.empty()) return;

    // Parse comma-separated list
    QStringList exec_list;
    std::istringstream ss(execs_csv);
    std::string token;
    while (std::getline(ss, token, ',')) {
        auto s = token.find_first_not_of(" \t");
        auto e = token.find_last_not_of(" \t");
        if (s != std::string::npos && e != std::string::npos) {
            exec_list.append(QString::fromStdString(token.substr(s, e - s + 1)));
        }
    }

    auto icon_cache = current_instance_root_.empty()
        ? std::filesystem::path{}
        : current_instance_root_ / "cache" / "thumbnails";
    right_panel_->exec_controls()->set_executables(exec_list, QString::fromStdString(default_exec), current_game_dir_, icon_cache);
    engine::Logger::instance().info("Populated " + std::to_string(exec_list.size()) + " executables for " + current_game_id_);
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

    // Auto-detect runtime from executable extension:
    //   .exe → Proton (Windows binary on Linux)
    //   anything else → Native
    std::unique_ptr<engine::Runtime> runtime;
    auto ext = exec_path.extension().string();
    if (!ext.empty() && ext[0] == '.') {
        auto lower_ext = ext.substr(1);
        // Case-insensitive compare
        for (auto& c : lower_ext) c = std::tolower(c);
        if (lower_ext == "exe") {
            runtime = std::make_unique<engine::ProtonRuntime>();
        }
    }
    if (!runtime) {
        runtime = std::make_unique<engine::NativeRuntime>();
    }

    engine::Logger::instance().info("Launching: " + exec_path.string() +
        " (runtime: " + runtime->name() + ", appid: " + std::to_string(steam_appid) + ")");

    if (!runtime->launch(exec_path, current_game_dir_, steam_appid)) {
        QMessageBox::warning(this, "Launch", "Failed to launch game.");
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
    connect(btn, &QToolButton::clicked, this, [this, full_path]() {
        launch_with_executable(full_path);
    });
    toolbar_shortcut_paths_.append(full_path);
    save_order();
    engine::Logger::instance().info("Added toolbar shortcut: " + full_path.toStdString());
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

    engine::Logger::instance().info("Created desktop shortcut: " + desktop_file.toStdString());
    QMessageBox::information(this, "Shortcut",
        "Desktop shortcut created:\n" + desktop_file);
}

std::filesystem::path MainWindow::app_state_path() const {
    if (current_instance_root_.empty()) return {};
    return current_instance_root_ / "config" / "app_state.dat";
}

void MainWindow::closeEvent(QCloseEvent* event) {
    save_app_state();
    QMainWindow::closeEvent(event);
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

    engine::Logger::instance().debug("App state saved");
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

    if (!geo.isEmpty()) restoreGeometry(geo);
    if (!win_state.isEmpty()) restoreState(win_state);
    if (main_splitter_ && !main_split.isEmpty()) main_splitter_->restoreState(main_split);
    if (console_splitter_ && !console_split.isEmpty()) console_splitter_->restoreState(console_split);
    if (mod_view_ && mod_view_->header() && !header_state.isEmpty()) {
        mod_view_->header()->restoreState(header_state);
    }

    engine::Logger::instance().debug("App state restored");
}

}  // namespace ui
