#include "ui/widgets/main_toolbar.h"

#include <QFrame>
#include <QBoxLayout>
#include <QIcon>
#include <QStyle>

namespace ui {

MainToolbar::MainToolbar(QWidget* parent)
    : QWidget(parent) {
    auto* settings_btn = add_gmm_button("Settings", "preferences-system");
    auto* instances_btn = add_gmm_button("Switch Instance", "computerr");

    connect(settings_btn, &QToolButton::clicked, this, &MainToolbar::settings_clicked);
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
    // Try the named icon from the desktop theme first, fall back to a standard icon
    auto theme_icon = QIcon::fromTheme(icon_name);
    btn->setIcon(theme_icon.isNull() ? style()->standardIcon(QStyle::SP_ComputerIcon) : theme_icon);
    btn->setAutoRaise(true);
    btn->setIconSize(QSize(24, 24));
    btn->setToolButtonStyle(Qt::ToolButtonIconOnly);
    gmm_buttons_.append(btn);
    return btn;
}

QToolButton* MainToolbar::add_exec_button(const QString& tooltip, const QIcon& icon) {
    auto* btn = new QToolButton(this);
    btn->setToolTip(tooltip);
    btn->setIcon(icon);
    btn->setAutoRaise(true);
    btn->setIconSize(QSize(24, 24));
    btn->setToolButtonStyle(Qt::ToolButtonIconOnly);
    exec_buttons_.append(btn);
    layout_->addWidget(btn);
    return btn;
}

}  // namespace ui
