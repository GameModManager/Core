#include "ui/main_window/main_window.h"
#include "ui/widgets/mod_list_model.h"
#include "ui/widgets/main_toolbar.h"
#include "ui/widgets/profile_bar.h"
#include "ui/widgets/mod_filter_bar.h"
#include "ui/widgets/column_toggle_header.h"
#include "ui/widgets/right_panel.h"
#include "ui/widgets/exec_controls_bar.h"
#include "ui/widgets/console_panel.h"
#include "ui/widgets/gmm_status_bar.h"
#include "ui/pipeline_worker.h"
#include "engine/log/logger.h"

#include <QHeaderView>
#include <QMessageBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTableView>
#include <QToolBar>
#include <QVBoxLayout>

namespace ui {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent) {
    setWindowTitle("GameModManager");
    resize(1200, 800);

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
        QMessageBox::information(this, "Settings", "Settings — coming soon");
    });
    connect(toolbar_, &MainToolbar::profiles_clicked, this, [this]() {
        QMessageBox::information(this, "Profiles", "Profiles — coming soon");
    });
    connect(toolbar_, &MainToolbar::instances_clicked, this, [this]() {
        QMessageBox::information(this, "Instances", "Switch Instance — coming soon");
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
        QMessageBox::information(this, "Create", "Create Separator — coming soon");
    });
    connect(profile_bar_, &ProfileBar::create_empty_mod_clicked, this, [this]() {
        QMessageBox::information(this, "Create", "Create Empty Mod — coming soon");
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
    mod_view_ = new QTableView(this);
    mod_view_->setModel(mod_model_);
    mod_view_->setSelectionBehavior(QAbstractItemView::SelectRows);
    mod_view_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    mod_view_->setDragDropMode(QAbstractItemView::InternalMove);
    mod_view_->setDragDropOverwriteMode(false);
    mod_view_->setSortingEnabled(true);
    mod_view_->verticalHeader()->setVisible(false);
    mod_view_->verticalHeader()->setDefaultSectionSize(24);

    auto* mod_header = new ColumnToggleHeaderView(Qt::Horizontal, mod_view_);
    mod_header->set_column_labels({"Enabled", "Name", "Version", "Status", "Priority"});
    mod_view_->setHorizontalHeader(mod_header);

    mod_header->setStretchLastSection(false);
    mod_header->setSectionsMovable(true);
    mod_header->setSectionResizeMode(ModListModel::Name, QHeaderView::Stretch);

    mod_header->setSortIndicatorShown(true);
    mod_header->setSortIndicator(ModListModel::Priority, Qt::DescendingOrder);

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

    connect(right_panel_->exec_controls(), &ExecControlsBar::run_clicked, this, [this]() {
        engine::Logger::instance().info("Run button clicked");
    });

    connect(right_panel_->exec_controls(), &ExecControlsBar::shortcut_to_toolbar, this, [this]() {
        engine::Logger::instance().info("Shortcut to toolbar requested");
    });

    connect(right_panel_->exec_controls(), &ExecControlsBar::shortcut_to_desktop, this, [this]() {
        engine::Logger::instance().info("Shortcut to desktop requested");
    });
}

void MainWindow::on_notification(const QString& title, const QString& message) {
    status_bar_->set_status(title + ": " + message);
}

void MainWindow::set_game_info(const std::string& game_id,
                                const std::string& game_display_name,
                                const std::string& profile_name) {
    current_game_id_ = game_id;
    current_game_name_ = game_display_name;
    current_profile_name_ = profile_name;
    update_title();
}

void MainWindow::update_title() {
    if (current_game_name_.empty()) {
        setWindowTitle("GameModManager");
    } else if (current_profile_name_.empty()) {
        setWindowTitle(("GameModManager — " + current_game_name_).c_str());
    } else {
        setWindowTitle(("GameModManager — " + current_profile_name_ + " — " + current_game_name_).c_str());
    }
}

}  // namespace ui
