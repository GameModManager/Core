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
  void show_notification(
      const QString &title, const QString &message,
      QSystemTrayIcon::MessageIcon icon = QSystemTrayIcon::Information);

  [[nodiscard]] bool is_visible() const;

signals:
  void activate_requested(); // user clicked tray icon
  void quit_requested();     // user selected quit from tray menu

private:
  QSystemTrayIcon *tray_icon_ = nullptr;
  QMenu *tray_menu_ = nullptr;
};

} // namespace ui
