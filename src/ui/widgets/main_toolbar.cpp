#include "ui/widgets/main_toolbar.h"

#include <QFrame>
#include <QBoxLayout>
#include <QStyle>

namespace ui {

MainToolbar::MainToolbar(QWidget* parent)
    : QWidget(parent) {
    auto* settings_btn = add_gmm_button("Settings", "preferences-system");
    auto* profiles_btn = add_gmm_button("Profiles", "user-bookmarks");
    auto* instances_btn = add_gmm_button("Switch Instance", "system-file-manager");

    connect(settings_btn, &QToolButton::clicked, this, &MainToolbar::settings_clicked);
    connect(profiles_btn, &QToolButton::clicked, this, &MainToolbar::profiles_clicked);
    connect(instances_btn, &QToolButton::clicked, this, &MainToolbar::instances_clicked);

    separator_ = new QFrame();
    separator_->setFrameShape(QFrame::VLine);
    separator_->setFrameShadow(QFrame::Sunken);

    layout_ = new QBoxLayout(QBoxLayout::LeftToRight, this);
    layout_->setContentsMargins(4, 2, 4, 2);
    layout_->setSpacing(2);

    for (auto* btn : gmm_buttons_) {
        btn->setToolButtonStyle(Qt::ToolButtonIconOnly);
        layout_->addWidget(btn);
    }

    layout_->addStretch(1);

    layout_->addWidget(separator_);

    for (auto* btn : exec_buttons_) {
        btn->setToolButtonStyle(Qt::ToolButtonIconOnly);
        layout_->addWidget(btn);
    }
}

void MainToolbar::set_vertical(bool vertical) {
    if (vertical_ == vertical) return;
    vertical_ = vertical;

    layout_->setDirection(vertical_ ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    separator_->setFrameShape(vertical_ ? QFrame::HLine : QFrame::VLine);

    if (vertical_) {
        setMinimumWidth(32);
        setMinimumHeight(0);
    } else {
        setMinimumHeight(32);
        setMinimumWidth(0);
    }
}

QToolButton* MainToolbar::add_gmm_button(const QString& tooltip, const QString& icon_name) {
    auto* btn = new QToolButton(this);
    btn->setToolTip(tooltip);
    btn->setIcon(style()->standardIcon(QStyle::SP_ComputerIcon));
    btn->setAutoRaise(true);
    btn->setIconSize(QSize(24, 24));
    btn->setToolButtonStyle(Qt::ToolButtonIconOnly);
    gmm_buttons_.append(btn);
    return btn;
}

QToolButton* MainToolbar::add_exec_button(const QString& tooltip, const QString& icon_name) {
    auto* btn = new QToolButton(this);
    btn->setToolTip(tooltip);
    btn->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    btn->setAutoRaise(true);
    btn->setIconSize(QSize(24, 24));
    btn->setToolButtonStyle(Qt::ToolButtonIconOnly);
    exec_buttons_.append(btn);
    return btn;
}

}  // namespace ui
