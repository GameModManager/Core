#include "ui/widgets/main_toolbar.h"

#include <QFrame>
#include <QBoxLayout>
#include <QIcon>
#include <QMenu>
#include <QStyle>

namespace ui {

MainToolbar::MainToolbar(QWidget* parent)
    : QWidget(parent) {
    auto* instances_btn = add_gmm_button("Switch Instance", "computer");
    auto* settings_btn = add_gmm_button("Settings", "preferences-system");

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

    // Right-click context menu to remove shortcut
    btn->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(btn, &QWidget::customContextMenuRequested, this, [this, btn](const QPoint& pos) {
        Q_UNUSED(pos);
        QMenu menu;
        menu.addAction(tr("Remove shortcut"));
        auto* action = menu.exec(btn->mapToGlobal(QPoint(0, btn->height())));
        if (action) {
            QString path = btn->property("exec_path").toString();
            remove_exec_button(btn);
            if (!path.isEmpty()) {
                emit shortcut_removed(path);
            }
        }
    });

    return btn;
}

void MainToolbar::remove_exec_button(QToolButton* btn) {
    if (!btn) return;
    exec_buttons_.removeOne(btn);
    layout_->removeWidget(btn);
    btn->deleteLater();
}

void MainToolbar::clear_exec_buttons() {
    while (!exec_buttons_.isEmpty()) {
        auto* btn = exec_buttons_.takeFirst();
        layout_->removeWidget(btn);
        btn->deleteLater();
    }
}

}  // namespace ui
