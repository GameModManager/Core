#pragma once

#include <QObject>
#include <QSystemTrayIcon>
#include <QString>

class QMenu;

namespace ui {

class MainWindow;

// ---------------------------------------------------------------------------
// SystemTrayManager - manages the system tray icon and its context menu.
// ---------------------------------------------------------------------------
class SystemTrayManager : public QObject {
  Q_OBJECT

public:
  explicit SystemTrayManager(MainWindow *parent = nullptr);

  void show();
  void hide();
  void
  show_notification(const QString &title, const QString &message,
                    QSystemTrayIcon::MessageIcon icon = QSystemTrayIcon::Information);

  [[nodiscard]] bool is_visible() const;

  // Whether this system has a working tray at all, probed once at construction.
  // False means the icon cannot be reached, so nothing may hide behind it -
  // see ui::tray::decide_tray_action.
  [[nodiscard]] bool is_available() const { return available_; }

signals:
  void activate_requested();  // user clicked tray icon
  void quit_requested();      // user selected quit from tray menu

private:
  QSystemTrayIcon *tray_icon_ = nullptr;
  QMenu *tray_menu_           = nullptr;
  bool available_             = false;
};

}  // namespace ui
