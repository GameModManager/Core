#include "ui/controllers/tab_mode_controller.h"
#include "ui/controllers/settings_controller.h"

#include <QDialog>
#include <QWidget>

#include "ui/settings/settings.h"
#include "ui/settings/settings_dialog.h"
#include "ui/widgets/main_tab_container.h"
#include "ui/widgets/pipeline_window.h"

namespace ui {

TabModeController::TabModeController(MainWindow *w, QObject *parent)
    : QObject(parent), w_(w) {
  // When a settings tab is removed (tab close button or the dialog's own
  // Close button), re-apply any settings that may have changed while it was
  // open and release the dialog. Other pages (PipelineWindow) are owned and
  // reused by MainWindow, so they are left alone.
  connect(w_->main_tab_container_, &MainTabContainer::view_tab_removed, this,
          [this](QWidget *page) {
            if (auto *dlg = qobject_cast<SettingsDialog *>(page)) {
              w_->settings_->apply_settings_changes();
              dlg->deleteLater();
            }
          });
}

void TabModeController::route_settings() {
  if (!Settings::instance().full_ui_mode()) {
    // Popup mode: unchanged behavior.
    w_->settings_->show_settings_dialog();
    return;
  }

  const QString key = QStringLiteral("settings");
  if (is_tab_open(key)) {
    w_->main_tab_container_->select_tab(key);
    return;
  }

  auto *dlg = new SettingsDialog(w_->style_manager_, w_->native_style_name_,
                                 w_->current_instance_root_, w_->plugin_loader_,
                                 w_);
  // Embed as a tab page: strip the top-level window flags so the dialog
  // renders as a plain page inside the tab.
  dlg->setWindowFlags(dlg->windowFlags() & ~(Qt::Window | Qt::Dialog));
  // Closing the dialog (Close button -> closeEvent, or Esc -> reject) drops
  // the tab; view_tab_removed handles the settings re-apply + release.
  connect(dlg, &SettingsDialog::closed, this, [this, key]() { close_tab(key); });
  connect(dlg, &QDialog::rejected, this, [this, key]() { close_tab(key); });
  // Toggling the mode inside the dialog updates the tab bar live.
  connect(dlg, &SettingsDialog::full_ui_mode_toggled, this,
          &TabModeController::on_mode_changed);
  w_->main_tab_container_->add_view_tab(dlg, tr("Settings"), key);
}

void TabModeController::route_pipeline() {
  if (!Settings::instance().full_ui_mode()) {
    // Popup mode: unchanged behavior.
    w_->settings_->show_pipeline_window();
    return;
  }

  const QString key = QStringLiteral("pipeline");
  if (is_tab_open(key)) {
    w_->main_tab_container_->select_tab(key);
    return;
  }

  if (!w_->pipeline_window_)
    w_->pipeline_window_ = new PipelineWindow(w_);
  auto *win = w_->pipeline_window_;
  // Embed as a tab page (same flag strip as the settings dialog). The window
  // is owned by MainWindow and reused: show_pipeline_window() restores the
  // top-level flag when it is shown as a popup again.
  win->setWindowFlags(win->windowFlags() & ~(Qt::Window | Qt::Dialog));
  // Esc inside the embedded window rejects the dialog: drop the tab so it
  // does not linger as an empty page. The window is reused across tab opens,
  // so install the connection only once.
  if (!esc_connected_.contains(win)) {
    connect(win, &QDialog::rejected, this, [this, key]() { close_tab(key); });
    esc_connected_.insert(win);
  }
  win->refresh();
  w_->main_tab_container_->add_view_tab(win, tr("Pipeline"), key);
}

void TabModeController::open_in_tab(QWidget *content, const QString &title,
                                    const QString &key) {
  if (!content)
    return;
  if (Settings::instance().full_ui_mode()) {
    w_->main_tab_container_->add_view_tab(content, title, key);
    return;
  }
  // Popup fallback: show as a standalone window.
  content->setWindowTitle(title);
  content->setWindowFlag(Qt::Window, true);
  content->show();
  content->raise();
  content->activateWindow();
}

void TabModeController::close_tab(const QString &key) {
  if (w_->main_tab_container_)
    w_->main_tab_container_->remove_view_tab(key);
}

bool TabModeController::is_tab_open(const QString &key) const {
  return w_->main_tab_container_ && w_->main_tab_container_->has_tab(key);
}

void TabModeController::on_mode_changed(bool full_ui_mode) {
  if (!w_->main_tab_container_)
    return;
  w_->main_tab_container_->update_tab_bar_visibility();
  if (!full_ui_mode)
    w_->main_tab_container_->close_all_view_tabs();
}

} // namespace ui

#include "moc_tab_mode_controller.cpp"