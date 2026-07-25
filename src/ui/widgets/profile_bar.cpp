#include "ui/widgets/profile_bar.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QStyle>
#include <QToolButton>

namespace ui {

ProfileBar::ProfileBar(QWidget* parent)
    : QWidget(parent) {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 2, 4, 2);
    layout->setSpacing(4);

    // Profile label + dropdown
    auto* profile_label = new QLabel("Profile:", this);
    layout->addWidget(profile_label);

    profile_combo_ = new QComboBox(this);
    profile_combo_->addItem("Default");
    profile_combo_->setMinimumWidth(120);
    layout->addWidget(profile_combo_);

    layout->addSpacing(8);

    // Import button
    import_btn_ = new QToolButton(this);
    import_btn_->setText("Import");
    import_btn_->setIcon(style()->standardIcon(QStyle::SP_ArrowDown));
    import_btn_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    import_btn_->setPopupMode(QToolButton::MenuButtonPopup);

    auto* import_menu = new QMenu(this);
    import_menu->addAction("Import from CSV");
    import_menu->addAction("Import from URL");
    import_btn_->setMenu(import_menu);
    layout->addWidget(import_btn_);

    // Export button (default action = export CSV, dropdown for other options)
    export_btn_ = new QToolButton(this);
    export_btn_->setText("Export to CSV");
    export_btn_->setIcon(style()->standardIcon(QStyle::SP_ArrowUp));
    export_btn_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    export_btn_->setPopupMode(QToolButton::MenuButtonPopup);

    auto* export_menu = new QMenu(this);
    export_menu->addAction("Export to CSV");
    export_menu->addAction("Export to URL");
    export_btn_->setMenu(export_menu);
    layout->addWidget(export_btn_);

    layout->addSpacing(8);

    // Create dropdown (separator, empty mod)
    create_btn_ = new QToolButton(this);
    create_btn_->setText("Create");
    create_btn_->setIcon(style()->standardIcon(QStyle::SP_FileDialogNewFolder));
    create_btn_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    create_btn_->setPopupMode(QToolButton::InstantPopup);

    auto* create_menu = new QMenu(this);
    create_menu->addAction("Create Separator");
    create_menu->addAction("Create Empty Mod");
    connect(create_menu->actions()[0], &QAction::triggered,
            this, &ProfileBar::create_separator_clicked);
    connect(create_menu->actions()[1], &QAction::triggered,
            this, &ProfileBar::create_empty_mod_clicked);
    create_btn_->setMenu(create_menu);
    layout->addWidget(create_btn_);

    layout->addStretch();

    connect(profile_combo_, &QComboBox::currentTextChanged,
            this, &ProfileBar::profile_changed);
    connect(import_btn_, &QToolButton::clicked,
            this, &ProfileBar::import_clicked);
    connect(export_btn_, &QToolButton::clicked,
            this, &ProfileBar::export_clicked);
}

}  // namespace ui
