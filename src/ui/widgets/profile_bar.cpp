#include "ui/widgets/profile_bar.h"

#include <QComboBox>
#include <QHBoxLayout>
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

    // Import button - empty stub for now
    import_btn_ = new QToolButton(this);
    import_btn_->setText(tr("Import"));
    import_btn_->setIcon(style()->standardIcon(QStyle::SP_ArrowDown));
    import_btn_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    import_btn_->setEnabled(false);
    layout->addWidget(import_btn_);

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
    connect(import_btn_, &QToolButton::clicked,
            this, &ProfileBar::import_clicked);
}

}  // namespace ui
