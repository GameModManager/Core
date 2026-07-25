#include "ui/widgets/exec_controls_bar.h"

#include <QComboBox>
#include <QGridLayout>
#include <QMenu>
#include <QStyle>
#include <QToolButton>

namespace ui {

ExecControlsBar::ExecControlsBar(QWidget* parent)
    : QWidget(parent) {
    auto* layout = new QGridLayout(this);
    layout->setContentsMargins(4, 2, 4, 2);
    layout->setSpacing(4);

    // Big combo dropdown — spans 2 rows
    exec_combo_ = new QComboBox(this);
    exec_combo_->setMinimumHeight(50);
    exec_combo_->setMinimumWidth(200);
    exec_combo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    exec_combo_->addItem("Select executable...");
    layout->addWidget(exec_combo_, 0, 0, 2, 1);

    // Run button — top right, same width as shortcut
    run_btn_ = new QToolButton(this);
    run_btn_->setText("Run");
    run_btn_->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    run_btn_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    run_btn_->setMinimumHeight(24);
    run_btn_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    layout->addWidget(run_btn_, 0, 1);

    // Shortcut button with dropdown — below Run
    shortcut_btn_ = new QToolButton(this);
    shortcut_btn_->setText("Shortcut");
    shortcut_btn_->setIcon(style()->standardIcon(QStyle::SP_FileLinkIcon));
    shortcut_btn_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    shortcut_btn_->setMinimumHeight(24);
    shortcut_btn_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    shortcut_btn_->setPopupMode(QToolButton::MenuButtonPopup);

    auto* shortcut_menu = new QMenu(this);
    shortcut_menu->addAction("Shortcut to Toolbar");
    shortcut_menu->addAction("Shortcut to Desktop");
    connect(shortcut_menu->actions()[0], &QAction::triggered,
            this, &ExecControlsBar::shortcut_to_toolbar);
    connect(shortcut_menu->actions()[1], &QAction::triggered,
            this, &ExecControlsBar::shortcut_to_desktop);
    shortcut_btn_->setMenu(shortcut_menu);
    layout->addWidget(shortcut_btn_, 1, 1);

    // Combo takes 70%, buttons take 30%
    layout->setColumnStretch(0, 7);
    layout->setColumnStretch(1, 3);

    connect(run_btn_, &QToolButton::clicked, this, &ExecControlsBar::run_clicked);
    // Default shortcut click = toolbar
    connect(shortcut_btn_, &QToolButton::clicked,
            this, &ExecControlsBar::shortcut_to_toolbar);
}

QString ExecControlsBar::current_executable() const {
    return exec_combo_->currentText();
}

}  // namespace ui
