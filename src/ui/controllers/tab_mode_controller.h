#pragma once

#include <QObject>
#include <QPointer>
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

  // Routes the Instance Statistics dialog: tab when Full UI mode is ON,
  // modal popup otherwise.
  void route_stats();

  // Routes the executable editor (Modify Executables): tab when Full UI mode
  // is ON, modal popup otherwise. In tab mode the editor has an explicit Save
  // button that applies the entries to ExecControlsBar and closes the tab;
  // popup mode keeps the OK/Cancel dialog behavior.
  void route_exec_entry();

  // Routes the Instance Options panel: tab when Full UI mode is ON, modal
  // popup otherwise. In tab mode the panel has an explicit Save button that
  // persists the runner to instance.toml and closes the tab; popup mode keeps
  // the modal InstanceOptionsDialog behavior.
  void route_instance_options();

  // Routes the instance switcher: tab when Full UI mode is ON, modal popup
  // otherwise. In tab mode selecting an instance switches immediately (full
  // app reload) and drops the tab; the create button runs the
  // GameSelectionWidget create flow and drops the tab on success.
  void route_instance_switcher();

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
  // Last current tab page. QTabWidget::currentChanged only reports the NEW
  // index, so the previous page is tracked here to detect switching AWAY
  // from the Settings tab (where the dialog-equivalent post-close side
  // effects must fire). QPointer auto-clears when the page is deleted.
  QPointer<QWidget> previous_page_;
};

} // namespace ui