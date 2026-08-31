#include "ui/controllers/tab_mode_controller.h"
#include "ui/controllers/launch_controller.h"
#include "ui/controllers/settings_controller.h"

#include <QDialog>
#include <QWidget>

#include "engine/core/instance/instance.h"
#include "engine/core/instance/instance_utils.h"
#include "ui/instance_options/instance_options_widget.h"
#include "ui/settings/settings.h"
#include "ui/settings/settings_content_widget.h"
#include "ui/widgets/exec_controls_bar.h"
#include "ui/widgets/executables_entry.h"
#include "ui/widgets/instance_switcher_content_widget.h"
#include "ui/widgets/main_tab_container.h"
#include "ui/widgets/mod_list_model.h"
#include "ui/widgets/pipeline_content_widget.h"
#include "ui/widgets/right_panel.h"
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
            } else if (auto *exec =
                           qobject_cast<Executables::ContentWidget *>(page)) {
              exec->deleteLater();
            } else if (auto *instance_options =
                           qobject_cast<InstanceOptionsWidget *>(page)) {
              instance_options->deleteLater();
            } else if (auto *switcher = qobject_cast<
                           InstanceSwitcherContentWidget *>(page)) {
              switcher->deleteLater();
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

void TabModeController::route_exec_entry() {
  if (!Settings::instance().full_ui_mode()) {
    // Popup mode: unchanged behavior (modal Executables::Dialog).
    w_->launch_->on_add_entry_requested();
    return;
  }

  const QString key = QStringLiteral("exec_entry");
  if (is_tab_open(key)) {
    w_->main_tab_container_->select_tab(key);
    return;
  }

  // Tab mode: embed a fresh Executables::ContentWidget. The editor never applies
  // anything on its own - the explicit Save button applies the entries to
  // ExecControlsBar (incrementally, the tab stays open for further edits) and
  // closes the tab; Cancel just closes the tab, discarding the edits.
  auto icon_cache = w_->cache_thumbnails_dir_path();
  auto existing = w_->right_panel_->exec_controls()->executable_entries();
  auto *content = new Executables::ContentWidget(w_->current_game_dir_,
                                             w_->launch_->output_mod_list(),
                                             existing, icon_cache, w_);
  connect(content, &Executables::ContentWidget::save_requested, this,
          [this, key, content]() {
            w_->launch_->apply_exec_entries(content->entries());
            close_tab(key);
          });
  connect(content, &Executables::ContentWidget::cancel_requested, this,
          [this, key]() { close_tab(key); });
  open_in_tab(content, tr("Modify Executables"), key);
}

void TabModeController::route_instance_options() {
  if (!Settings::instance().full_ui_mode()) {
    // Popup mode: unchanged behavior (modal InstanceOptionsDialog).
    w_->launch_->show_instance_options();
    return;
  }

  const QString key = QStringLiteral("instance_options");
  if (is_tab_open(key)) {
    w_->main_tab_container_->select_tab(key);
    return;
  }

  // No instance loaded: the popup path shows the "no instance" info box.
  auto params = w_->launch_->instance_options_params();
  if (!params.valid) {
    w_->launch_->show_instance_options();
    return;
  }

  // Tab mode: embed a fresh InstanceOptionsWidget. The widget never persists
  // the runner on its own - the explicit Save button persists it to
  // instance.toml and closes the tab; Close just closes the tab, discarding
  // the runner change (the deploy strategy persists immediately on change,
  // matching the popup behavior).
  auto *content = new InstanceOptionsWidget(
      w_->platform_, w_->plugin_loader_, params.game_id, params.game_name,
      params.game_dir, params.steam_appid, params.instance_root,
      params.current_runner, params.deploy_strategy, params.deploy_config, w_);
  // The deploy management section must flush the deferred disable queue before
  // Force re-deploy / Remove, exactly like the launch path does (the deploy
  // reads on-disk sentinels, so queued toggles must be applied first).
  content->set_flush_deferred_disable_queue(
      [this]() { w_->launch_->flush_deferred_disable_queue(); });
  connect(content, &InstanceOptionsWidget::save_requested, this,
          [this, key, content]() {
            auto runner = content->selected_runner();
            engine::Instance write =
                engine::Instance::from_root(w_->current_instance_root_);
            write.read_toml();
            write.write_key("proton_runner", runner);
            w_->current_instance_ = write;
            close_tab(key);
          });
  connect(content, &InstanceOptionsWidget::cancel_requested, this,
          [this, key]() { close_tab(key); });
  open_in_tab(content, tr("Instance Options"), key);
}

void TabModeController::route_instance_switcher() {
  if (!Settings::instance().full_ui_mode()) {
    // Popup mode: unchanged behavior (modal InstanceSwitcherDialog).
    w_->settings_->show_instance_switcher();
    return;
  }

  const QString key = QStringLiteral("instance_switcher");
  if (is_tab_open(key)) {
    w_->main_tab_container_->select_tab(key);
    return;
  }

  // Tab mode: embed a fresh InstanceSwitcherContentWidget. Selecting an
  // instance switches immediately (single click) and drops the tab; the
  // create button runs the GameSelectionWidget create flow and drops the tab
  // on success. The tab stays open when the user cancels the create flow or
  // the switch fails.
  auto instances_dir = engine::default_instances_dir();
  auto *content = new InstanceSwitcherContentWidget(w_->plugin_loader_, w_);
  content->set_immediate_switch(true);
  content->load_instances(instances_dir.string());
  connect(content, &InstanceSwitcherContentWidget::instance_selected, this,
          [this, key](const QString &name) {
            if (w_->settings_->switch_to_instance(name))
              close_tab(key);
          });
  connect(content, &InstanceSwitcherContentWidget::create_new_instance, this,
          [this, key]() {
            if (w_->settings_->create_new_instance())
              close_tab(key);
          });
  open_in_tab(content, tr("Switch Instance"), key);
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