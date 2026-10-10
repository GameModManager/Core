#include "ui/widgets/main_toolbar.h"

#include "ui/theme/icon_manager.h"

#include <QFrame>
#include <QBoxLayout>
#include <QIcon>
#include <QMenu>
#include <QStyle>

namespace ui {

MainToolbar::MainToolbar(QWidget *parent) : QWidget(parent) {
  setWhatsThis(
      tr("The toolbar. Its buttons apply to the whole instance; the ones on the "
         "right are shortcuts to the executables you have set up."));

  auto *instances_btn = add_gmm_button("Switch Instance", "computer");
  auto *settings_btn  = add_gmm_button("Settings", "preferences-system");

  connect(settings_btn, &QToolButton::clicked, this, &MainToolbar::settings_clicked);
  connect(instances_btn, &QToolButton::clicked, this, &MainToolbar::instances_clicked);

  separator_ = new QFrame();
  separator_->setFrameShape(QFrame::VLine);
  separator_->setFrameShadow(QFrame::Sunken);

  layout_ = new QBoxLayout(QBoxLayout::LeftToRight, this);
  layout_->setContentsMargins(4, 2, 4, 2);
  layout_->setSpacing(2);

  for (auto *btn : gmm_buttons_) {
    btn->setToolButtonStyle(Qt::ToolButtonIconOnly);
    layout_->addWidget(btn);
  }

  layout_->addStretch(1);

  layout_->addWidget(separator_);

  for (auto *btn : exec_buttons_) {
    btn->setToolButtonStyle(Qt::ToolButtonIconOnly);
    layout_->addWidget(btn);
  }
}

void MainToolbar::set_vertical(bool vertical) {
  if (vertical_ == vertical)
    return;
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

void MainToolbar::set_icon_size(int size) {
  current_icon_size_ = size;
  for (auto *btn : gmm_buttons_)
    btn->setIconSize(QSize(size, size));
  for (auto *btn : exec_buttons_)
    btn->setIconSize(QSize(size, size));
}

QToolButton *MainToolbar::add_gmm_button(const QString &tooltip,
                                         const QString &icon_name) {
  auto *btn = new QToolButton(this);
  btn->setToolTip(tooltip);
  btn->setIcon(
      engine::IconManager::instance().resolve_icon(icon_name, QStyle::SP_ComputerIcon));
  btn->setProperty("gmm_icon_name", icon_name);
  btn->setAutoRaise(true);
  btn->setIconSize(QSize(current_icon_size_, current_icon_size_));
  btn->setToolButtonStyle(Qt::ToolButtonIconOnly);
  gmm_buttons_.append(btn);
  return btn;
}

void MainToolbar::reapply_icons() {
  for (auto *btn : gmm_buttons_) {
    if (btn == instance_options_button_)
      continue;
    const QString name = btn->property("gmm_icon_name").toString();
    if (name.isEmpty())
      continue;
    btn->setIcon(
        engine::IconManager::instance().resolve_icon(name, QStyle::SP_ComputerIcon));
  }
  if (instance_options_button_) {
    instance_options_button_->setIcon(engine::IconManager::instance().resolve_icon(
        "proton", QStyle::SP_ComputerIcon));
  }
}

QToolButton *MainToolbar::add_instance_options_button(const QIcon &icon) {
  instance_options_button_ = new QToolButton(this);
  instance_options_button_->setToolTip(tr("Instance Options"));
  instance_options_button_->setIcon(icon);
  instance_options_button_->setAutoRaise(true);
  instance_options_button_->setIconSize(QSize(current_icon_size_, current_icon_size_));
  instance_options_button_->setToolButtonStyle(Qt::ToolButtonIconOnly);
  // Body click opens the Instance Options panel; the arrow opens the
  // dropdown menu. MO2 setupActionMenu (mainwindow.cpp:746-753) switches a
  // toolbar button that owns a menu to InstantPopup so the WHOLE button opens
  // the menu instead of press-and-hold on a separate arrow: a toolbar button
  // is not a split control the way a combo box is, there is no second, more
  // common action on it worth preserving. Set in set_instance_options_menu
  // rather than here - a button with no menu yet has to stay clickable, or
  // the panel entry point dies before the menu is ever attached.
  instance_options_button_->setPopupMode(QToolButton::MenuButtonPopup);
  gmm_buttons_.append(instance_options_button_);

  // Belongs with the settings buttons on the left, not the exec shortcuts.
  // Second slot: after Switch Instance, before Settings.
  if (layout_) {
    layout_->insertWidget(1, instance_options_button_);
  }

  connect(instance_options_button_, &QToolButton::clicked, this,
          &MainToolbar::instance_options_clicked);
  return instance_options_button_;
}

void MainToolbar::set_instance_options_menu(QMenu *menu) {
  if (!instance_options_button_)
    return;
  instance_options_button_->setMenu(menu);
  // With a menu attached the whole button opens it (MO2 setupActionMenu); with
  // no menu it stays a plain click so the panel still opens.
  instance_options_button_->setPopupMode(menu ? QToolButton::InstantPopup
                                              : QToolButton::MenuButtonPopup);
}

QToolButton *MainToolbar::add_exec_button(const QString &tooltip, const QIcon &icon) {
  auto *btn = new QToolButton(this);
  btn->setToolTip(tooltip);
  btn->setIcon(icon);
  btn->setAutoRaise(true);
  btn->setIconSize(QSize(current_icon_size_, current_icon_size_));
  btn->setToolButtonStyle(Qt::ToolButtonIconOnly);
  exec_buttons_.append(btn);
  layout_->addWidget(btn);

  // Right-click context menu to remove the shortcut. MO2 names the action it
  // is about to remove in the entry itself
  // (MainWindow::toolBar_customContextMenuRequested,
  // mainwindow.cpp:3788-3795: "Remove '%1' from the toolbar"), so a
  // right-click on a toolbar with several pinned executables says which one
  // goes. The bare "Remove shortcut" said nothing.
  btn->setContextMenuPolicy(Qt::CustomContextMenu);
  const QString label = tooltip;
  connect(btn, &QWidget::customContextMenuRequested, this,
          [this, btn, label](const QPoint &pos) {
            Q_UNUSED(pos);
            QMenu menu;
            auto *remove =
                menu.addAction(tr("Remove '%1' from the toolbar").arg(label));
            if (menu.exec(btn->mapToGlobal(QPoint(0, btn->height()))) != remove)
              return;
            QString path = btn->property("exec_path").toString();
            remove_exec_button(btn);
            if (!path.isEmpty()) {
              emit shortcut_removed(path);
            }
          });

  return btn;
}

QList<ExecShortcut> MainToolbar::exec_shortcuts() const {
  QList<ExecShortcut> out;
  out.reserve(exec_buttons_.size());
  for (const auto *btn : exec_buttons_) {
    out.append(ExecShortcut{btn->property("exec_path").toString(), btn->toolTip(),
                            btn->icon()});
  }
  return out;
}

void MainToolbar::remove_exec_button(QToolButton *btn) {
  if (!btn)
    return;
  exec_buttons_.removeOne(btn);
  layout_->removeWidget(btn);
  btn->deleteLater();
}

void MainToolbar::clear_exec_buttons() {
  while (!exec_buttons_.isEmpty()) {
    auto *btn = exec_buttons_.takeFirst();
    layout_->removeWidget(btn);
    btn->deleteLater();
  }
}

}  // namespace ui
