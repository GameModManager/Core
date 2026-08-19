#include "ui/controllers/tab_mode_controller.h"
#include "ui/controllers/settings_controller.h"

#include <QDialog>
#include <QWidget>

#include "ui/settings/settings.h"
#include "ui/settings/settings_content_widget.h"
#include "ui/widgets/main_tab_container.h"
#include "ui/widgets/mod_list_model.h"
#include "ui/widgets/pipeline_content_widget.h"
#include "ui/widgets/stats_content_widget.h"

namespace ui {

TabModeController::TabModeController(MainWindow *w, QObject *parent)
    : QObject(parent), w_(w) {
  // When a settings tab is removed (tab close button or Full UI mode turned
  // OFF), re-apply any settings that may have changed while it was open and
  // release the panel. Stats and Pipeline tabs are owned by this controller,
  // so they are released on removal.
  connect(w_->main_tab_container_, &MainTabContainer::view_tab_removed, this,
          [this](QWidget *page) {
            if (auto *content = qobject_cast<SettingsContentWidget *>(page)) {
              w_->settings_->apply_settings_changes();
              content->deleteLater();
            } else if (auto *stats = qobject_cast<StatsContentWidget *>(page)) {
              stats->deleteLater();
            } else if (auto *pipeline =
                           qobject_cast<PipelineContentWidget *>(page)) {
              pipeline->deleteLater();
            }
          });

  // Switching AWAY from the Settings tab fires the same side effects that the
  // popup dialog runs after exec() returns (SettingsController::
  // apply_settings_changes), so changes take effect live while the tab stays
  // open. QTabWidget::currentChanged reports the NEW index only, so the
  // previous page is tracked in previous_page_.
  connect(w_->main_tab_container_, &QTabWidget::currentChanged, this,
          [this](int) {
            if (qobject_cast<SettingsContentWidget *>(previous_page_.data())) {
              w_->settings_->apply_settings_changes();
            }
            previous_page_ = w_->main_tab_container_->currentWidget();
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

  // Full UI mode: the mode-agnostic settings panel is embedded as a tab page
  // instead of a modal dialog. The plain QWidget needs no window-flag strip.
  auto *content = new SettingsContentWidget(
      w_->style_manager_, w_->native_style_name_, w_->current_instance_root_,
      w_->plugin_loader_, w_);
  // Toggling the mode inside the panel updates the tab bar live.
  connect(content, &SettingsContentWidget::full_ui_mode_toggled, this,
          &TabModeController::on_mode_changed);
  // Closing the tab (close button, or close_all_view_tabs when the mode is
  // turned OFF) is handled by the view_tab_removed connection above: it
  // re-applies the settings and releases the panel.
  open_in_tab(content, tr("Settings"), key);
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

  // Tab mode: embed a fresh PipelineContentWidget. The tab container does not
  // delete pages on close; view_tab_removed releases it (see constructor).
  auto *content = new PipelineContentWidget(w_);
  w_->main_tab_container_->add_view_tab(content, tr("Pipeline"), key);
}

void TabModeController::route_stats() {
  if (!Settings::instance().full_ui_mode()) {
    // Popup mode: unchanged behavior.
    w_->settings_->show_instance_statistics();
    return;
  }

  const QString key = QStringLiteral("stats");
  if (is_tab_open(key)) {
    w_->main_tab_container_->select_tab(key);
    return;
  }

  // No instance loaded: the popup path shows the "no instance" info box.
  if (w_->current_instance_root_.empty()) {
    w_->settings_->show_instance_statistics();
    return;
  }

  auto cache_dir = w_->cache_dir_path();
  int total_mods = 0;
  for (const auto &m : w_->mod_model_->mods()) {
    if (!m.is_separator && !m.is_overwrite)
      ++total_mods;
  }

  auto *stats = new StatsContentWidget(w_->current_instance_root_, cache_dir,
                                       total_mods, w_);
  // The widget's own Close button drops the tab; the tab bar's close button
  // is handled by view_tab_removed (deleteLater). The widget refreshes on
  // every show, so re-activating the tab re-reads the current sizes.
  connect(stats, &StatsContentWidget::close_requested, this,
          [this, key]() { close_tab(key); });
  w_->main_tab_container_->add_view_tab(stats, tr("Instance Statistics"), key);
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