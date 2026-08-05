#include "ui/widgets/profile_bar.h"

#include "engine/theme/icon_manager.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QStyle>
#include <QToolButton>

namespace ui {

ProfileBar::ProfileBar(QWidget* parent)
    : QWidget(parent) {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 2, 4, 2);
    layout->setSpacing(4);

    // Profile label + dropdown
    auto* profile_label = new QLabel(tr("Profile:"), this);
    layout->addWidget(profile_label);

    profile_combo_ = new QComboBox(this);
    profile_combo_->addItem(tr("Default"));
    profile_combo_->setMinimumWidth(120);
    layout->addWidget(profile_combo_, 1);

    layout->addSpacing(8);

    // Open folders button - hosts all the important instance paths
    // (MO2's openFolderMenu). Icon-only, no text.
    folders_btn_ = new QToolButton(this);
    folders_btn_->setIcon(engine::IconManager::instance().resolve_icon(
        "document-open-folder", QStyle::SP_DirOpenIcon));
    folders_btn_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    folders_btn_->setPopupMode(QToolButton::MenuButtonPopup);

    auto* folders_menu = new QMenu(this);
    auto add_folder = [this, folders_menu](const QString& text, FolderKind kind) {
        auto* action = folders_menu->addAction(text);
        connect(action, &QAction::triggered, this,
                [this, kind]() { emit open_folder_requested(kind); });
        return action;
    };

    add_folder(tr("Open Game folder"), FolderKind::Game);
    add_folder(tr("Open MyGames folder"), FolderKind::MyGames);
    add_folder(tr("Open INIs folder"), FolderKind::Inis);
    folders_menu->addSeparator();
    add_folder(tr("Open Instance folder"), FolderKind::Instance);
    add_folder(tr("Open Mods folder"), FolderKind::Mods);
    add_folder(tr("Open Profile folder"), FolderKind::Profile);
    add_folder(tr("Open Downloads folder"), FolderKind::Downloads);
    folders_menu->addSeparator();
    auto* install_action = add_folder(tr("Open GMM Install folder"), FolderKind::Install);
    install_action->setEnabled(false);  // app isn't installed anywhere
    add_folder(tr("Open GMM Plugins folder"), FolderKind::Plugins);
    add_folder(tr("Open GMM Themes folder"), FolderKind::Themes);
    add_folder(tr("Open GMM Logs folder"), FolderKind::Logs);

    folders_btn_->setMenu(folders_menu);
    layout->addWidget(folders_btn_);

    connect(folders_btn_, &QToolButton::clicked, this, [this]() {
        if (auto* m = folders_btn_->menu())
            m->exec(folders_btn_->mapToGlobal(QPoint(0, folders_btn_->height())));
    });

    // Combined Import/Export button (replaces the old export to CSV)
    QPixmap up_px = style()->standardIcon(QStyle::SP_ArrowUp).pixmap(12, 12);
    QPixmap down_px = style()->standardIcon(QStyle::SP_ArrowDown).pixmap(12, 12);
    QPixmap combined(16, 16);
    combined.fill(Qt::transparent);
    QPainter p(&combined);
    p.drawPixmap(2, 0, up_px);
    p.drawPixmap(2, 4, down_px);
    p.end();

    import_export_btn_ = new QToolButton(this);
    import_export_btn_->setIcon(QIcon(combined));
    import_export_btn_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    import_export_btn_->setPopupMode(QToolButton::MenuButtonPopup);

    auto* import_export_menu = new QMenu(this);
    auto* export_action = import_export_menu->addAction(tr("Export modlist"));
    auto* import_action = import_export_menu->addAction(tr("Import modlist"));
    import_export_btn_->setMenu(import_export_menu);
    layout->addWidget(import_export_btn_);

    connect(import_export_btn_, &QToolButton::clicked, this, [this]() {
        if (auto* m = import_export_btn_->menu())
            m->exec(import_export_btn_->mapToGlobal(QPoint(0, import_export_btn_->height())));
    });
    connect(export_action, &QAction::triggered,
            this, &ProfileBar::export_modlist_clicked);
    connect(import_action, &QAction::triggered,
            this, &ProfileBar::import_modlist_clicked);

    layout->addSpacing(8);

    // Create dropdown (icon only, no text)
    create_btn_ = new QToolButton(this);
    create_btn_->setIcon(style()->standardIcon(QStyle::SP_FileDialogNewFolder));
    create_btn_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    create_btn_->setPopupMode(QToolButton::MenuButtonPopup);

    auto* create_menu = new QMenu(this);
    create_menu->addAction(tr("Create Separator"));
    create_menu->addAction(tr("Create Empty Mod"));
    connect(create_menu->actions()[0], &QAction::triggered,
            this, &ProfileBar::create_separator_clicked);
    connect(create_menu->actions()[1], &QAction::triggered,
            this, &ProfileBar::create_empty_mod_clicked);
    create_btn_->setMenu(create_menu);
    layout->addWidget(create_btn_);

    connect(create_btn_, &QToolButton::clicked, this, [this]() {
        if (auto* m = create_btn_->menu())
            m->exec(create_btn_->mapToGlobal(QPoint(0, create_btn_->height())));
    });

    connect(profile_combo_, &QComboBox::currentTextChanged,
            this, &ProfileBar::profile_changed);
}

}  // namespace ui
