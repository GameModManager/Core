#pragma once

#include <QObject>
#include <QSet>
#include <QString>

#include "ui/main_window/main_window.h"

class QWidget;

namespace ui {

// Routes toolbar/menu actions to either popup dialogs or in-window tabs based
// on the Full UI mode preference (Settings::full_ui_mode()).
//
// Full UI mode ON:  Settings / Pipeline open as tabs inside MainTabContainer.
// Full UI mode OFF: identical to the pre-tab behavior (modal dialog / popup
// window). A friend of MainWindow so it can reach the shared members
// (settings_, pipeline_window_, main_tab_container_).
class TabModeController : public QObject {
  Q_OBJECT
public:
  explicit TabModeController(MainWindow *w, QObject *parent = nullptr);

  // Routes the Settings dialog: tab when Full UI mode is ON, modal popup
  // otherwise.
  void route_settings();

  // Routes the Pipeline window: tab when Full UI mode is ON, popup window
  // otherwise.
  void route_pipeline();

  // Generic tab routing: opens `content` in a tab titled `title` (registered
  // under `key`) when Full UI mode is ON; shows it as a standalone popup
  // window otherwise. An already-open tab with the same key is selected
  // instead of duplicated.
  void open_in_tab(QWidget *content, const QString &title, const QString &key);

  // Closes the dynamic tab registered under `key` (no-op when absent).
  void close_tab(const QString &key);

  // True when a dynamic tab registered under `key` is open.
  bool is_tab_open(const QString &key) const;

  // Handles a Full UI mode toggle: refreshes the tab bar visibility and
  // closes all dynamic tabs when the mode is turned OFF.
  void on_mode_changed(bool full_ui_mode);

private:
  MainWindow *w_ = nullptr;
  // Reused windows (PipelineWindow) that already have their Esc -> close_tab
  // connection installed, so re-embedding them never stacks duplicates. Raw
  // pointers are safe: the windows are MainWindow-lifetime, as is this
  // controller.
  QSet<QWidget *> esc_connected_;
};

} // namespace ui