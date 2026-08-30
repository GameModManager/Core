#include "ui/system_tray/system_tray_manager.h"

#include "ui/main_window/main_window.h"

#include <QApplication>
#include <QMenu>
#include <QSystemTrayIcon>

namespace ui {

SystemTrayManager::SystemTrayManager(MainWindow *parent)
    : QObject(parent) {
  tray_icon_ = new QSystemTrayIcon(this);
  tray_icon_->setIcon(QApplication::windowIcon());
  tray_icon_->setToolTip("GameModManager");

  tray_menu_ = new QMenu();
  auto *show_action = tray_menu_->addAction("Show");
  connect(show_action, &QAction::triggered, this,
          &SystemTrayManager::activate_requested);

  tray_menu_->addSeparator();

  auto *quit_action = tray_menu_->addAction("Quit");
  connect(quit_action, &QAction::triggered, this,
          &SystemTrayManager::quit_requested);

  tray_icon_->setContextMenu(tray_menu_);

  connect(tray_icon_, &QSystemTrayIcon::activated, this,
          [this](QSystemTrayIcon::ActivationReason reason) {
            if (reason == QSystemTrayIcon::Trigger ||
                reason == QSystemTrayIcon::DoubleClick) {
              emit activate_requested();
            }
          });
}

void SystemTrayManager::show() {
  tray_icon_->show();
}

void SystemTrayManager::hide() {
  tray_icon_->hide();
}

void SystemTrayManager::show_notification(
    const QString &title, const QString &message,
    QSystemTrayIcon::MessageIcon icon) {
  tray_icon_->showMessage(title, message, icon);
}

bool SystemTrayManager::is_visible() const {
  return tray_icon_->isVisible();
}

} // namespace ui
