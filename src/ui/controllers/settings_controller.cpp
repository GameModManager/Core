#include "ui/controllers/settings_controller.h"
#include "ui/controllers/downloads_controller.h"
#include "ui/controllers/launch_controller.h"
#include "ui/controllers/mod_list_controller.h"
#include "ui/controllers/tab_mode_controller.h"

#include <QApplication>
#include <QCheckBox>
#include <QDesktopServices>
#include <QFileDialog>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QProcess>
#include <QSplitter>
#include <QStatusBar>
#include <QTableWidget>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidget>
#include <QUrl>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <vector>

#include "engine/deploy/strategy.h"
#include "engine/game/detect/game_detector.h"
#include "engine/core/events/event_bus.h"
#include "engine/core/util/fs_utils.h"
#include "engine/core/instance/instance.h"
#include "engine/pipeline/plugin_host/category_factory.h"
#include "engine/core/instance/instance_utils.h"
#include "engine/core/log/logger.h"
#include "engine/source/nxm/managed_games.h"
#include "ui/nxm/nxm_ipc.h"
#include "engine/mod/overwrite/overwrite_utils.h"
#include "engine/pipeline/extract_stage.h"
#include "engine/pipeline/fomod_stage.h"
#include "engine/pipeline/install_stage.h"
#include "engine/pipeline/plugin_claim_stage.h"
#include "engine/pipeline/sync_stage.h"
#include "engine/pipeline/plugin_host/plugin_loader.h"
#include "engine/game/registry/game_knowledge.h"
#include "engine/sort/sort_registry.h"
#include "engine/source/loverslab_provider.h"
#include "engine/source/nexus_provider.h"
#include "engine/source/steam_workshop_provider.h"
#include "ui/theme/icon_manager.h"
#include "ui/theme/style_manager.h"
#include "engine/core/trace/trace_recorder.h"
#include "ui/fomod/fomod_wizard_dialog.h"
#include "ui/game_selection/game_selection_widget.h"
#include "ui/install/install_name_dialog.h"
#include "ui/main_window/main_window.h"
#include "ui/overwrite/query_overwrite_dialog.h"
#include "ui/panels/tab_panels.h"
#include "ui/workers/pipeline_worker.h"
#include "ui/settings/settings.h"
#include "ui/settings/settings_dialog.h"
#include "ui/widgets/console_panel.h"
#include "ui/widgets/debug_window.h"
#include "ui/widgets/exec_controls_bar.h"
#include "ui/widgets/gmm_status_bar.h"
#include "ui/widgets/instance_statistics_dialog.h"
#include "ui/widgets/instance_switcher_dialog.h"
#include "ui/widgets/main_toolbar.h"
#include "ui/widgets/menu_bar.h"
#include "ui/widgets/mod_list_model.h"
#include "ui/widgets/mod_table_view.h"
#include "ui/widgets/pipeline_window.h"
#include "ui/widgets/right_panel.h"

#ifdef GMM_PLATFORM_LINUX
#include "engine/deploy/launch/overlay_launcher.h"
#include "platform/linux/linux_platform.h"
#endif

namespace ui {

SettingsController::SettingsController(MainWindow *w, QObject *parent)
    : QObject(parent), w_(w) {}

void SettingsController::set_game_info(
    const std::string &game_id, const std::string &game_display_name,
    const std::string &profile_name, const std::filesystem::path &game_dir,
    const std::filesystem::path &instance_root) {
  // Clear previous instance state before switching
  w_->console_->clear();
  w_->downloads_->hide_install_progress();
  w_->toolbar_->clear_exec_buttons();
  w_->toolbar_shortcut_paths_.clear();
  w_->right_panel_->exec_controls()->clear_executables();
  w_->nxm_links_.clear(); // NXM links are instance-scoped

  w_->current_game_id_ = game_id;
  w_->current_game_name_ = game_display_name;
  w_->current_profile_name_ = profile_name;
  w_->current_game_dir_ = game_dir;
  w_->current_instance_root_ = instance_root;
  if (!instance_root.empty()) {
    // Per-instance category registry (categories.dat): user edits made in the
    // Categories dialog persist here. load() replaces the plugin-merged set
    // with the persisted one (which already contains the plugin defaults); a
    // missing file leaves the registry unchanged (first run).
    engine::CategoryFactory::instance().load(instance_root / "categories.dat");
    w_->current_instance_ = engine::Instance::from_root(instance_root);
    w_->current_instance_.read_toml();
    w_->conflict_cache_path_ =
        w_->current_instance_.path_for(engine::InstanceKind::Cache) /
        "conflict_cache.json";
  }

  // The Overwrite folder may carry CI-duplicate directories (Meshes/ +
  // meshes/) left by an earlier session on the case-sensitive Linux fs: the
  // game's raw writes split one logical dir across casings. Fold them back
  // once per instance load (deferred so the switch is never blocked - it is
  // idempotent, a clean Overwrite costs one listing).
  if (!instance_root.empty() && w_->knowledge_ &&
      w_->knowledge_->get(w_->current_game_id_, "case_sensitive", "true") ==
          "false") {
    auto ow = w_->current_instance_.path_for(engine::InstanceKind::Overwrite);
    QTimer::singleShot(0, this, [this, ow]() {
      if (engine::normalize_overwrite_casing(ow) > 0) {
        engine::Logger::instance().debug(
            "Overwrite: healed case-insensitive directory duplicates in " +
            ow.string());
      }
    });
  }

  // P5 (MO2 createOverwriteDirectories parity): pre-create the Overwrite
  // mapping root <overwrite>/<mods_subpath> so the game's first write finds
  // its target dir already present. Deferred + idempotent, never blocks load.
  if (!instance_root.empty() && w_->knowledge_) {
    auto ow_root =
        w_->current_instance_.path_for(engine::InstanceKind::Overwrite);
    auto ow_subpath =
        w_->knowledge_->get(w_->current_game_id_, "mods_subpath", "");
    if (!ow_subpath.empty()) {
      auto mapping_root = ow_root / ow_subpath;
      QTimer::singleShot(0, this, [this, mapping_root]() {
        std::error_code ec;
        std::filesystem::create_directories(mapping_root, ec);
        if (ec) {
          engine::Logger::instance().warn(
              "Overwrite: failed to pre-create mapping root " +
              mapping_root.string() + ": " + ec.message());
        }
      });
    }
  }

  // Any in-flight conflict scan belongs to the previous instance: bump the
  // generation so its result is dropped when it lands, and discard queued
  // requests/invalidations for the old game. running_ stays true so the
  // worker thread never has more than one scan queued (they serialize on the
  // shared conflict cache file); the new game's first recompute queues behind
  // it and on_conflict_scan_finished() drains the queue on the stale result.
  w_->conflict_scan_generation_ = w_->conflict_scan_generation_ + 1;
  w_->conflict_scan_pending_ = false;
  w_->conflict_scan_pending_follow_ups_.clear();
  w_->conflict_scan_active_follow_ups_.clear();
  w_->conflict_invalidate_pending_.clear();
  // The same for any in-flight mod scan: it belongs to the previous
  // instance's mods dir, so its result must be dropped (load_mods_from_game
  // also bumps when it launches a fresh scan).
  w_->mod_scan_generation_ = w_->mod_scan_generation_ + 1;
  // Restore the persisted per-instance executable selection BEFORE the
  // combo is populated, so set_executables can land on it instead of
  // defaulting to the first real entry.
  restore_exec_selection();
  w_->update_title();

  // Populate the profile selector from the instance's profiles dir. Resolves
  // the startup profile: the passed-in name when it exists, else the saved
  // default profile, else the first profile (see
  // ModListController::refresh_profiles).
  w_->mod_list_->refresh_profiles();

  // (Loaded plugin list is logged once by PluginLoader::load_directory)

  if (!game_dir.empty() && w_->knowledge_) {
    w_->mod_list_->update_status_bar_for_game();

    // Rebuild right-panel tabs for w_ game
    if (w_->plugin_loader_)
      w_->right_panel_->set_capabilities(&w_->plugin_loader_->capabilities());
    w_->right_panel_->set_game(w_->current_game_id_);

    // Restore the last selected right-panel tab for this instance (Issue
    // #21). current_instance_ was read from instance.toml above; empty or
    // unsupported values fall back to the first tab.
    w_->right_panel_->restore_tab(w_->current_instance_.info().last_tab);

    // Connect conflicts tab signals (tab created during set_game)
    auto *ct = w_->right_panel_->conflicts_tab();
    if (ct) {
      connect(ct, &ui::ConflictsTab::image_diff_requested, w_->mod_list_.get(),
              &ModListController::on_image_diff_requested);
    }

    // Wire the freshly created Data tab: resolve real file paths and run
    // Open/Execute/Preview/Add-as-Executable/Mod Info/Hide from the
    // context menu, plus "Reveal in file manager" and refresh.
    w_->mod_list_->wire_data_tab();

    // The Downloads tab was also freshly created by set_game: load its
    // manifest, point it at w_ instance's downloads dir (which starts
    // the directory watchdog) and connect its signals. Without w_, a
    // switched instance shows a dead tab.
    w_->downloads_->wire_downloads_tab();

    // The Saves tab (if the game supports it) was freshly created too:
    // point it at the game's saves dir, connect refresh/delete, and run an
    // initial scan.
    w_->downloads_->wire_saves_tab();

    w_->mod_list_->load_mods_from_game();
    // Plugin-DB disk load runs CONCURRENTLY with the mod scan (T6/P8.5):
    // the two independent startup loads overlap instead of the plugin DB
    // waiting for the scan to land and then reading the same disk
    // sequentially. refresh_plugins_tab() adopts the preload when it's
    // ready, and falls back to a synchronous read otherwise.
    w_->mod_list_->launch_plugin_db_preload();
    w_->launch_->load_executables();
    w_->launch_->populate_executables();

    // Check if a sort provider is available for w_ game
    bool has_sort_provider =
        engine::SortRegistry::instance().get_provider(game_id) != nullptr;
    w_->menu_bar_->set_sort_available(has_sort_provider);

    // Configure pipeline for w_ game instance
    engine::PipelineContext ctx;
    ctx.game_dir = w_->current_game_dir_;
    ctx.meta_dir = w_->current_instance_root_ / "meta";
    ctx.mods_dir = w_->mods_dir_path();
    // Metadata format inside installed mod folders: MO2's meta.ini by
    // default, the game's XML file (Isaac's metadata.xml) if the game
    // registered the metadata_file hook.
    ctx.metadata_file =
        w_->knowledge_->get(w_->current_game_id_, "metadata_file", "meta.ini");

    // When an install targets an existing mod folder, ask the user how to
    // proceed (Merge/Replace/Rename/Cancel) instead of silently replacing.
    // The pipeline runs on a worker thread; ask_overwrite marshals the
    // modal dialog onto the main thread. Backup defaults to checked,
    // matching MO2's QueryOverwriteDialog::BACKUP_YES default.
    ctx.overwrite_query_cb = [this](const std::string &mod_name) {
      // The user dialog supersedes the progress popup (MO2 does the
      // same: the progress dialog is only visible while it can show
      // progress, not while a decision is pending).
      w_->downloads_->hide_install_progress();
      return ui::ask_overwrite(QString::fromStdString(mod_name),
                               /*default_backup=*/true, w_);
    };

    // A FOMOD archive opens the install wizard. It drives the
    // pipeline-owned FomodViewModel directly; ask_fomod marshals the modal
    // onto the main thread like ask_overwrite. Settings gate the
    // previous-choice restore and the image preview.
    ctx.fomod_query_cb =
        [this](const std::shared_ptr<engine::FomodViewModel> &view_model,
               const std::filesystem::path &content_root,
               const std::string &suggested_name,
               const std::string &previous_choices) {
          w_->downloads_->hide_install_progress();
          auto &s = Settings::instance();
          // FOMOD wizard behavior comes from the FomodInstaller plugin's
          // declared settings (register_settings_tab), which render inline
          // under Plugins > FOMOD Installer. Fall back to the legacy core
          // keys when the plugin isn't loaded.
          bool always_restore = s.always_restore_fomod_choices();
          bool show_images = s.show_fomod_images();
          if (w_->plugin_loader_) {
            for (const auto &p : w_->plugin_loader_->plugins()) {
              if (p.settings_tab.title != "FOMOD") continue;
              const QString basename = QString::fromStdString(
                  std::filesystem::path(p.path).filename().string());
              always_restore =
                  s.plugin_setting(basename, "Restore previous choices", "1") == "1";
              show_images =
                  s.plugin_setting(basename, "Show FOMOD images", "1") == "1";
              break;
            }
          }
          return ui::ask_fomod(
              view_model, content_root, suggested_name, previous_choices,
              always_restore, show_images, w_);
        };

    // Non-FOMOD installs confirm the mod name before copying (MO2's
    // SimpleInstallDialog). The suggested name is the Downloads-tab display
    // name (typically the Nexus name); the dialog's dropdown also offers a
    // cleaned archive-stem derivation and the full archive filename.
    // Canceling aborts the install.
    ctx.name_query_cb = [this](const std::string &suggested_name,
                               const std::string &archive_filename) {
      w_->downloads_->hide_install_progress();
      return ui::ask_install_name(suggested_name, archive_filename, w_);
    };

    // Set up deploy strategy. The effective strategy (per-instance
    // instance.toml override, else the game plugin's knowledge default) is
    // the single source of truth for the launch path and the Deploy
    // Management selector; report it here so the startup log matches the
    // configured strategy instead of the host's overlay capability.
    // Snapshot the game knowledge with a null guard (same pattern as the
    // launch path) so a missing knowledge registry degrades to defaults
    // instead of dereferencing null.
    engine::GameKnowledge knowledge =
        w_->knowledge_ ? *w_->knowledge_ : engine::GameKnowledge();
    const std::string deploy_strategy_name = engine::effective_deploy_strategy(
        w_->current_instance_root_, knowledge, w_->current_game_id_);
    ctx.deploy_prefix =
        knowledge.get(w_->current_game_id_, "deploy_prefix", "Data");
    auto inc_id = knowledge.get(w_->current_game_id_,
                                "deploy_include_mod_id", "false");
    ctx.deploy_include_mod_id = (inc_id == "true");
    bool case_sensitive =
        knowledge.get(w_->current_game_id_, "case_sensitive", "true") !=
        "false";
    std::unique_ptr<engine::DeploymentStrategy> deploy_strategy;
    std::string deploy_strategy_label;
#ifdef GMM_PLATFORM_LINUX
    if (deploy_strategy_name == engine::kDeployStrategyOverlayFs &&
        engine::OverlayFsLauncher::is_supported(w_->overwrite_dir_path())) {
      // OverlayFS: deploy symlinks into staging dir (not game_dir)
      auto staging = w_->current_instance_root_ / ".gmm_staging";
      ctx.staging_dir = staging;
      auto ovl_strat = std::make_unique<engine::OverlayFsDeployStrategy>(
          staging, case_sensitive);
      w_->staging_dir_ = staging;
      deploy_strategy = std::move(ovl_strat);
      deploy_strategy_label = "OverlayFS";
    } else
#endif
    {
      deploy_strategy =
          std::make_unique<engine::SymlinkStrategy>(case_sensitive);
      deploy_strategy_label =
          (deploy_strategy_name == engine::kDeployStrategyDirect) ? "Direct"
                                                                  : "Symlink";
    }
    engine::Logger::instance().info("Deploy strategy: " + deploy_strategy_label);
    ctx.deploy_strategy = deploy_strategy.get();

    // Build the install pipeline from the 3-stage template.  A plugin
    // stage claim (StageRegistry) wins over the core implementation for
    // the same stage name; stages with no implementation at all stay out
    // of the pipeline and render as "Not implemented" in the pipeline
    // window.  Installs are intentionally download-free: they unpack the
    // already-downloaded archive and copy it into the mods folder, with a
    // FOMOD-detection branch that aborts with a warning until FOMOD
    // installers are supported.
    auto claim_for = [this](const std::string &stage_name)
        -> std::optional<engine::StageClaim> {
      if (!w_->plugin_loader_)
        return std::nullopt;
      std::optional<engine::StageClaim> best;
      for (const auto &c : w_->plugin_loader_->stage_registry().claims()) {
        if (c.stage_name != stage_name) continue;
        // An empty game_id is a wildcard claim matching any game; at equal
        // priority a game-specific claim wins over a wildcard.
        const bool wildcard = c.game_id.empty();
        if (!wildcard && c.game_id != w_->current_game_id_) continue;
        if (!best) {
          best = c;
        } else if (c.priority > best->priority ||
                   (c.priority == best->priority && best->game_id.empty() &&
                    !wildcard)) {
          best = c;
        }
      }
      return best;
    };

    std::vector<std::pair<const char *,
                          std::function<std::unique_ptr<engine::Stage>()>>>
        core_makers = {
            {"Extract",
             [] { return std::make_unique<engine::ExtractStage>(); }},
            {"Fomod", [] { return std::make_unique<engine::FomodStage>(); }},
            {"Install",
             [] { return std::make_unique<engine::InstallStage>(); }},
        };
    static const char *kInstallStages[] = {
        "Extract",
        "Fomod",
        "Install",
    };

    auto pipeline = std::make_unique<engine::Pipeline>();
    pipeline->set_context(ctx);
    pipeline->set_flow_id("install");

    std::vector<engine::TraceStage> flow_stages;
    flow_stages.reserve(3);
    for (const char *stage_name : kInstallStages) {
      auto claim = claim_for(stage_name);
      std::unique_ptr<engine::Stage> impl;
      std::string origin = "core";

      if (claim) {
        impl = std::make_unique<engine::PluginClaimStage>(
            stage_name, claim->plugin_id, claim->handler);
        origin = claim->plugin_id;
      } else {
        for (const auto &[name, maker] : core_makers) {
          if (std::strcmp(name, stage_name) == 0) {
            impl = maker();
            break;
          }
        }
      }

      engine::TraceStage ts;
      ts.name = stage_name;
      ts.origin = origin;
      ts.implemented = (impl != nullptr);
      if (impl)
        ts.description = impl->description();
      flow_stages.push_back(std::move(ts));
      if (impl)
        pipeline->add_stage(std::move(impl));
    }
    engine::TraceRecorder::instance().declare_flow(
        "install", "Mod install pipeline", std::move(flow_stages));

    // The download flow is fetch-only (downloads are decoupled from
    // installs); PipelineWorker::download_mod runs it.
    engine::TraceRecorder::instance().declare_flow(
        "download", "Mod download",
        {
            {"Fetch", "core",
             "Downloads the archive into the instance downloads dir "
             "(pause/resume supported)"},
        });

    // Declare the sort + launch flows eagerly too - the pipeline window
    // must show the full stage list (and who provides what) before either
    // flow has ever run.  sort_mods()/launch_with_executable() only
    // begin_flow() at run time.
    engine::TraceRecorder::instance().declare_flow(
        "sort", "Auto-sort",
        {
            {"Gather mod info", "core",
             "Collects folder names, display names and workshop IDs from the "
             "mod list"},
            {"Run sort provider", "core",
             "Invokes the game's registered sort provider"},
            {"Apply order", "core",
             "Reorders the mod list per the provider's result"},
            {"Save order", "core", "Writes the new load order to disk"},
        });
    engine::TraceRecorder::instance().declare_flow(
        "launch", "Game launch",
        {
            {"Sync disk order", "core",
             "Writes the UI's load order to disk before launch"},
            {"Prepare launch environment", "core",
             "Sets up the overlay / Proton environment and deploys enabled "
             "mods"},
            {"Launch executable", "core",
             "Starts the game through the launch tier chain"},
            {"Monitor process", "core",
             "Watches the running game and captures writes on exit"},
        });

    w_->pipeline_thread_->worker()->set_pipeline(std::move(pipeline));
    w_->pipeline_thread_->worker()->set_context(ctx);

    // Keep strategy alive for the lifetime of w_ session
    w_->deploy_strategy_ = std::move(deploy_strategy);

    // Populate Tools menu with game-specific tools
    if (w_->plugin_loader_) {
      w_->menu_bar_->update_tools_for_game(
          w_->current_game_id_,
          w_->plugin_loader_->tool_registry().tools_for_game(
              w_->current_game_id_));
    }

    // Prompt to register w_ game for nxm:// handling if not already managed
    prompt_nxm_registration();
  } else {
    // No game to load here. Any in-flight mod scan's result was just
    // dropped by the generation bump above, so clear the loading flag it
    // would otherwise leave stuck.
    w_->loading_ = false;
  }

  // Restore saved app state now that w_->current_instance_root_ is known
  restore_app_state();

  // Sync process tree checkbox/tree with restored state (overlay was created
  // before restore)
  if (w_->process_tree_checkbox_)
    w_->process_tree_checkbox_->setChecked(w_->show_process_tree_);
  if (w_->process_tree_)
    w_->process_tree_->setVisible(w_->show_process_tree_);

  // The Downloads tab is wired (manifest, downloads dir + watchdog, signal
  // connections) by wire_downloads_tab() right after the tab is created.

  // Show debug window if debugging.enabled flag exists
  if (!w_->current_instance_root_.empty()) {
    auto flag = w_->current_instance_root_ / "debugging.enabled";
    if (std::filesystem::exists(flag)) {
      if (!w_->debug_window_) {
        w_->debug_window_ = new DebugWindow(
            w_->current_instance_root_, w_->current_game_id_,
            w_->current_game_name_, w_->plugin_loader_,
            [this]() {
              if (w_->style_manager_)
                w_->style_manager_->reload_current();
            },
            w_);
      }
      w_->debug_window_->show();
      w_->debug_window_->raise();
    } else if (w_->debug_window_) {
      w_->debug_window_->hide();
    }
  }

  refresh_recent_instances();

  // Self-heal the OS-level nxm:// handler once per session, after the window
  // is shown (deferred so the modal doesn't appear mid-construction).
  QTimer::singleShot(0, this, &SettingsController::ensure_nxm_handler_default);
}

void SettingsController::setup_menu_bar() {
  w_->menu_bar_ = new AppMenuBar(w_);
  w_->menu_bar_->setContextMenuPolicy(Qt::PreventContextMenu);
  w_->setMenuWidget(w_->menu_bar_);
}

void SettingsController::connect_menu_actions() {
  // --- File ---
  connect(w_->menu_bar_, &AppMenuBar::new_instance_requested, this,
          [this]() { w_->tab_mode_->route_instance_switcher(); });
  connect(w_->menu_bar_, &AppMenuBar::open_instance_requested, this,
          [this]() { w_->tab_mode_->route_instance_switcher(); });
  connect(w_->menu_bar_, &AppMenuBar::recent_instance_selected, this,
          [this](const QString &name) { switch_to_instance(name); });
  connect(w_->menu_bar_, &AppMenuBar::import_mods_requested, this, [this]() {
    if (w_->current_instance_root_.empty())
      return;
    const QStringList paths = QFileDialog::getOpenFileNames(
        w_, tr("Import Mod Archives"), QString(),
        tr("Archives (*.zip *.7z *.rar *.tar *.gz);;All files (*)"));
    if (paths.isEmpty())
      return;
    w_->mod_list_->import_archives(paths);
  });
  connect(w_->menu_bar_, &AppMenuBar::export_mods_requested, this,
          [this]() { w_->mod_list_->export_modlist(); });
  connect(w_->menu_bar_, &AppMenuBar::settings_requested, w_->tab_mode_.get(),
          &TabModeController::route_settings);
  connect(w_->menu_bar_, &AppMenuBar::exit_requested, this,
          [this]() { QApplication::quit(); });

  // --- Edit ---
  connect(w_->menu_bar_, &AppMenuBar::select_all_requested, this,
          [this]() { w_->mod_view_->selectAll(); });
  connect(w_->menu_bar_, &AppMenuBar::deselect_all_requested, this,
          [this]() { w_->mod_view_->clearSelection(); });
  connect(w_->menu_bar_, &AppMenuBar::enable_selected_requested, this,
          [this]() {
            auto sel = w_->mod_view_->selectionModel()->selectedRows();
            for (const auto &idx : sel) {
              w_->mod_model_->setData(idx, Qt::Checked, Qt::CheckStateRole);
            }
            engine::Logger::instance().debug(
                "Enabled " + std::to_string(sel.size()) + " mods");
          });
  connect(w_->menu_bar_, &AppMenuBar::disable_selected_requested, this,
          [this]() {
            auto sel = w_->mod_view_->selectionModel()->selectedRows();
            for (const auto &idx : sel) {
              w_->mod_model_->setData(idx, Qt::Unchecked, Qt::CheckStateRole);
            }
            engine::Logger::instance().debug(
                "Disabled " + std::to_string(sel.size()) + " mods");
          });
  connect(w_->menu_bar_, &AppMenuBar::priority_up_requested, this,
          [this]() { w_->mod_list_->priority_move_selected(-1); });
  connect(w_->menu_bar_, &AppMenuBar::priority_down_requested, this,
          [this]() { w_->mod_list_->priority_move_selected(+1); });

  // --- View ---
  connect(w_->menu_bar_, &AppMenuBar::toggle_toolbar, this,
          [this](bool visible) { w_->toolbar_area_->setVisible(visible); });
  connect(w_->menu_bar_, &AppMenuBar::toggle_status_bar, this,
          [this](bool visible) { w_->statusBar()->setVisible(visible); });
  connect(w_->menu_bar_, &AppMenuBar::toggle_console, this,
          [this](bool visible) {
            if (visible) {
              w_->console_splitter_->setSizes({700, 200});
            } else {
              w_->console_splitter_->setSizes({700, 0});
            }
          });
  connect(w_->menu_bar_, &AppMenuBar::pipeline_requested, w_->tab_mode_.get(),
          &TabModeController::route_pipeline);
  connect(w_->status_bar_, &GmmStatusBar::pipeline_clicked, w_->tab_mode_.get(),
          &TabModeController::route_pipeline);
  connect(w_->menu_bar_, &AppMenuBar::refresh_requested, this, [this]() {
    if (w_->current_game_id_.empty())
      return;
    w_->mod_list_->load_mods_from_game();
  });

  connect(w_->menu_bar_, &AppMenuBar::icon_size_requested, this,
          [this](int size) {
            w_->icon_size_ = size;
            w_->toolbar_area_->setIconSize(QSize(size, size));
            w_->toolbar_->set_icon_size(size);
          });

  // --- Tools ---
  connect(w_->menu_bar_, &AppMenuBar::tool_requested, this,
          [this](const QString &tool_id, const QString &game_id) {
            if (!w_->plugin_loader_)
              return;
            auto *tool = w_->plugin_loader_->tool_registry().get_tool(
                game_id.toStdString(), tool_id.toStdString());
            if (!tool) {
              engine::Logger::instance().warn(
                  "Tool not found: " + tool_id.toStdString() +
                  " for game: " + game_id.toStdString());
              return;
            }
            engine::Logger::instance().debug(
                "Running tool: " + tool_id.toStdString() +
                " for game: " + game_id.toStdString());
            if (tool_id == QStringLiteral("loot")) {
              // LOOT is an advisory tool the engine drives itself: build a
              // LootRequest from the current plugin DB and run gmm_lootcli off
              // the UI thread (PLAN.md §7.1).
              w_->mod_list_->run_loot_sort();
              return;
            }
            if (tool->invoke_fn) {
              tool->invoke_fn(tool->invoke_user_data);
            } else {
              // Fallback: try launching the executable
              for (const auto &path : tool->search_paths) {
                auto exe_path =
                    std::filesystem::path(path) / tool->executable_name;
                std::error_code ec;
                if (std::filesystem::exists(exe_path, ec)) {
                  QProcess::startDetached(
                      QString::fromStdString(exe_path.string()), QStringList());
                  return;
                }
              }
              engine::Logger::instance().warn("Tool executable not found: " +
                                              tool->executable_name);
            }
          });

  // --- Help ---
  connect(w_->menu_bar_, &AppMenuBar::about_requested, this, [this]() {
    QMessageBox::about(w_, tr("About GameModManager"),
                       "<h3>GameModManager</h3>"
                       "<p>Version " VERSION "</p>"
                       "<p>" +
                           tr("Cross-platform game mod manager with multi-repo "
                              "plugin support.") +
                           "</p>");
  });
  connect(w_->menu_bar_, &AppMenuBar::about_qt_requested, this,
          [this]() { QMessageBox::aboutQt(w_, tr("About Qt")); });
  connect(w_->menu_bar_, &AppMenuBar::instance_statistics_requested,
          w_->tab_mode_.get(), &TabModeController::route_stats);
}

void SettingsController::save_app_state() {
  auto path = w_->app_state_path();
  if (path.empty())
    return;

  std::ofstream out(path, std::ios::binary);
  if (!out)
    return;

  // Write window geometry + state
  auto geo = w_->saveGeometry().toBase64();
  auto win_state = w_->saveState().toBase64();
  auto main_split = w_->main_splitter_
                        ? w_->main_splitter_->saveState().toBase64()
                        : QByteArray();
  auto console_split = w_->console_splitter_
                           ? w_->console_splitter_->saveState().toBase64()
                           : QByteArray();
  auto header_state = w_->mod_view_ && w_->mod_view_->header()
                          ? w_->mod_view_->header()->saveState().toBase64()
                          : QByteArray();

  auto write_ba = [&](const QByteArray &ba) {
    uint32_t len = static_cast<uint32_t>(ba.size());
    out.write(reinterpret_cast<const char *>(&len), sizeof(len));
    if (len > 0)
      out.write(ba.constData(), len);
  };

  write_ba(geo);
  write_ba(win_state);
  write_ba(main_split);
  write_ba(console_split);
  write_ba(header_state);

  // Save right panel table header states (column widths, order, visibility)
  QJsonObject header_states;
  if (w_->right_panel_) {
    auto *tw = w_->right_panel_->tab_widget();
    for (int i = 0; i < tw->count(); ++i) {
      auto *tab = tw->widget(i);
      auto *table = tab->findChild<QTableWidget *>();
      if (table && table->horizontalHeader()) {
        auto state = table->horizontalHeader()->saveState().toBase64();
        if (!state.isEmpty()) {
          header_states[tw->tabText(i)] = QString::fromUtf8(state);
        }
      }
    }
  }
  header_states["_process_tree_visible"] = w_->show_process_tree_;
  header_states["icon_size"] = w_->icon_size_;
  if (w_->right_panel_ && w_->right_panel_->exec_controls()) {
    auto cur_exec = w_->right_panel_->exec_controls()->current_executable();
    if (!cur_exec.isEmpty())
      header_states["selected_exec"] = cur_exec;
  }
  // Match read_ba() below: every block in w_ file is stored base64.
  // (Historical note: w_ used to be written raw while read_ba() decoded
  // fromBase64, which silently corrupted the whole extra block - selected
  // executable, icon size, and right-panel header states never restored.)
  QByteArray extra =
      QJsonDocument(header_states).toJson(QJsonDocument::Compact).toBase64();
  write_ba(extra);
}

void SettingsController::restore_app_state() {
  auto path = w_->app_state_path();
  if (!std::filesystem::exists(path))
    return;

  std::ifstream in(path, std::ios::binary);
  if (!in)
    return;

  auto read_ba = [&]() -> QByteArray {
    uint32_t len = 0;
    if (!in.read(reinterpret_cast<char *>(&len), sizeof(len)) || len == 0)
      return {};
    std::vector<char> buf(len);
    if (!in.read(buf.data(), len))
      return {};
    return QByteArray::fromBase64(QByteArray(buf.data(), len));
  };

  auto geo = read_ba();
  auto win_state = read_ba();
  auto main_split = read_ba();
  auto console_split = read_ba();
  auto header_state = read_ba();
  (void)header_state; // mod-list header layout is NOT restored from disk:
                      // a state saved with a different column count shifts
                      // the old layout (Stretch/Interactive) onto the wrong
                      // columns. The header always uses the setup in
                      // setup_mod_view() instead.

  if (!geo.isEmpty())
    w_->pending_geometry_ = geo;
  if (!win_state.isEmpty())
    w_->restoreState(win_state);
  if (w_->main_splitter_ && !main_split.isEmpty())
    w_->main_splitter_->restoreState(main_split);
  if (w_->console_splitter_ && !console_split.isEmpty())
    w_->console_splitter_->restoreState(console_split);

  // Restore right panel table header states (column widths, order, visibility)
  auto obj = read_app_state_extra();
  if (!obj.isEmpty() && w_->right_panel_) {
    // Restore process tree visibility (prefixed with _ to avoid tab-name
    // collision)
    if (obj.contains("_process_tree_visible"))
      w_->show_process_tree_ = obj["_process_tree_visible"].toBool();
    // Restore toolbar icon size (Small/Medium/Large)
    if (obj.contains("icon_size")) {
      w_->icon_size_ = obj["icon_size"].toInt(24);
      w_->menu_bar_->set_icon_size(w_->icon_size_);
      if (w_->toolbar_area_)
        w_->toolbar_area_->setIconSize(QSize(w_->icon_size_, w_->icon_size_));
      if (w_->toolbar_)
        w_->toolbar_->set_icon_size(w_->icon_size_);
    }
    auto *tw = w_->right_panel_->tab_widget();
    for (int i = 0; i < tw->count(); ++i) {
      auto key = tw->tabText(i);
      if (!obj.contains(key))
        continue;
      auto *tab = tw->widget(i);
      auto *table = tab->findChild<QTableWidget *>();
      if (table && table->horizontalHeader()) {
        auto state = QByteArray::fromBase64(obj[key].toString().toUtf8());
        if (!state.isEmpty())
          table->horizontalHeader()->restoreState(state);
        // Re-apply desired stretch modes so restoreState
        // doesn't permanently override them from old sessions
        if (key == "Data") {
          auto *h = table->horizontalHeader();
          h->setStretchLastSection(false);
          h->setSectionResizeMode(0, QHeaderView::Stretch);
          h->setSectionResizeMode(1, QHeaderView::Interactive);
          h->setSectionResizeMode(2, QHeaderView::Interactive);
        } else if (key == "Downloads") {
          auto *h = table->horizontalHeader();
          h->setStretchLastSection(false);
          h->setSectionResizeMode(0, QHeaderView::Stretch);
          h->setSectionResizeMode(1, QHeaderView::Interactive);
          h->setSectionResizeMode(2, QHeaderView::Interactive);
          h->setSectionResizeMode(3, QHeaderView::Interactive);
        }
      }
    }
  }
}

QJsonObject SettingsController::read_app_state_extra() const {
  QJsonObject empty;
  auto path = w_->app_state_path();
  if (!std::filesystem::exists(path))
    return empty;
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return empty;

  auto read_ba = [&]() -> QByteArray {
    uint32_t len = 0;
    if (!in.read(reinterpret_cast<char *>(&len), sizeof(len)) || len == 0)
      return {};
    std::vector<char> buf(len);
    if (!in.read(buf.data(), len))
      return {};
    return QByteArray::fromBase64(QByteArray(buf.data(), len));
  };

  // Skip the five fixed blocks (geometry, window state, splitters, header)
  for (int i = 0; i < 5; ++i)
    read_ba();

  uint32_t extra_len = 0;
  if (!in.read(reinterpret_cast<char *>(&extra_len), sizeof(extra_len)) ||
      extra_len == 0)
    return empty;
  std::vector<char> buf(extra_len);
  if (!in.read(buf.data(), extra_len))
    return empty;
  QByteArray raw(buf.data(), extra_len);
  auto doc = QJsonDocument::fromJson(QByteArray::fromBase64(raw));
  if (!doc.isObject())
    doc = QJsonDocument::fromJson(raw); // legacy: extra written raw
  return doc.object();
}

void SettingsController::restore_exec_selection() {
  auto obj = read_app_state_extra();
  if (obj.contains("selected_exec"))
    w_->pending_exec_selection_ = obj["selected_exec"].toString();
}

void SettingsController::prompt_nxm_registration() {
  if (!w_->managed_games_ || !w_->plugin_loader_ ||
      w_->current_game_id_.empty())
    return;

  // Already registered? Skip.
  if (w_->managed_games_->is_managed(w_->current_game_id_))
    return;

  // Find the nexus_domain for w_ game from the loaded plugin identity
  const QString nexus_domain = w_->mod_list_->current_nexus_domain();

  // No nexus_domain means w_ game doesn't support Nexus Mods at all
  if (nexus_domain.isEmpty())
    return;

  QMessageBox msg(w_);
  msg.setWindowTitle(tr("Nexus Mods Downloads"));
  msg.setText(tr("Do you want to enable Nexus Mods downloads for <b>%1</b>?")
                  .arg(QString::fromStdString(w_->current_game_name_)));
  msg.setTextFormat(Qt::RichText);
  msg.setIcon(QMessageBox::Question);
  msg.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
  msg.setDefaultButton(QMessageBox::Yes);
  auto reply = msg.exec();

  if (reply == QMessageBox::Yes) {
    w_->managed_games_->add_source(
        w_->current_game_id_, engine::GameSource{"nexus", "nexusmods.com",
                                                 nexus_domain.toStdString()});

    engine::Logger::instance().debug(
        "Registered " + w_->current_game_id_ +
        " for Nexus Mods downloads (nexusmods.com)");
  }
}

void SettingsController::ensure_nxm_handler_default() {
  if (w_->nxm_handler_check_done_)
    return;
  w_->nxm_handler_check_done_ = true;

#ifdef GMM_PLATFORM_LINUX
  // Respect a permanent "don't ask again" choice
  if (Settings::instance().nxm_handler_check() == "dont_ask")
    return;

  // Self-heal: if we're still the default handler, nothing to do
  if (engine::LinuxPlatform::is_nxm_handler_registered()) {
    engine::Logger::instance().debug(
        "nxm:// handler check: GameModManager is the default");
    return;
  }

  auto app_path = std::filesystem::path(
      QCoreApplication::applicationFilePath().toStdString());

  engine::Logger::instance().info(
      "nxm:// handler check: GameModManager is NOT the default — prompting");
  QMessageBox msg(w_);
  msg.setWindowTitle(tr("NXM Protocol Handler"));
  msg.setText(tr("GameModManager is no longer the default app for "
                 "<b>nxm://</b> download links from Nexus Mods.\n\n"
                 "Make it the default again?"));
  msg.setTextFormat(Qt::RichText);
  msg.setIcon(QMessageBox::Question);
  auto *yes = msg.addButton(tr("Yes"), QMessageBox::YesRole);
  auto *dont_show = msg.addButton(tr("Don't show"), QMessageBox::ActionRole);
  auto *no = msg.addButton(tr("No"), QMessageBox::NoRole);
  msg.setDefaultButton(yes);
  msg.exec();

  if (msg.clickedButton() == yes) {
    (void)engine::LinuxPlatform::register_nxm_handler(app_path);
    (void)engine::LinuxPlatform::register_gmm_handler(app_path);
    engine::Logger::instance().info(
        "nxm:// handler registered: GameModManager");
  } else if (msg.clickedButton() == dont_show) {
    Settings::instance().set_nxm_handler_check("dont_ask");
    engine::Logger::instance().debug(
        "nxm:// handler check suppressed (don't show again)");
  } else {
    engine::Logger::instance().debug(
        "nxm:// handler check declined; will ask next launch");
  }
#else
  (void)0;
#endif
}

void SettingsController::show_settings_dialog() {
  SettingsDialog dlg(w_->style_manager_, w_->native_style_name_,
                     w_->current_instance_root_, w_->plugin_loader_, w_);
  dlg.exec();
  apply_settings_changes();
}

void SettingsController::apply_settings_changes() {
  // Per-folder path overrides may have changed in the dialog.
  if (!w_->current_instance_root_.empty()) {
    w_->current_instance_ =
        engine::Instance::from_root(w_->current_instance_root_);
    w_->current_instance_.read_toml();
  }
  // The icon-pack (and theme) settings may have changed in the dialog:
  // re-sync IconManager and re-apply the persistent window/toolbar icons.
  // Context menus build their icons on demand, so they pick it up already.
  {
    auto &icon_mgr = engine::IconManager::instance();
    icon_mgr.set_current_theme(Settings::instance().theme().toStdString());
    icon_mgr.set_mode(Settings::instance().icon_pack().toStdString());
    w_->toolbar_->reapply_icons();
    qApp->setWindowIcon(icon_mgr.resolve_icon("gmm-logo"));
  }
  // The separator-scrollbar setting may have changed in the dialog.
  if (w_->mod_view_)
    w_->mod_view_->apply_scrollbar_policy();
  // The per-instance nesting setting may have changed in the dialog.
  apply_nesting_setting();
  // The compact-downloads setting may have changed in the dialog.
  if (auto *dt = w_->right_panel_->downloads_tab())
    dt->apply_compact_style();
  // The Nexus queue-downloads setting may have changed in the dialog: push
  // the new value into the fetch pool.
  w_->pipeline_thread_->worker()->set_nexus_queue_downloads(
      Settings::instance().nexus_queue_downloads());
}

void SettingsController::apply_nesting_setting() {
  if (w_->current_instance_root_.empty())
    return;
  const auto key =
      QString::fromStdString(w_->current_instance_root_.filename().string());
  w_->mod_model_->set_nesting_enabled(Settings::instance().modlist_nested(key));
}

void SettingsController::show_instance_statistics() {
  if (w_->current_instance_root_.empty()) {
    QMessageBox::information(w_, tr("Instance Statistics"),
                             tr("No instance is currently loaded."));
    return;
  }

  auto cache_dir = w_->cache_dir_path();
  int total_mods = 0;
  for (const auto &m : w_->mod_model_->mods()) {
    if (!m.is_separator && !m.is_overwrite)
      ++total_mods;
  }

  InstanceStatisticsDialog dlg(w_->current_instance_root_, cache_dir,
                               total_mods, w_);
  dlg.exec();
}

void SettingsController::show_pipeline_window() {
  if (!w_->pipeline_window_) {
    w_->pipeline_window_ = new PipelineWindow(w_);
  }
  // Restore the top-level window flag in case the window was previously
  // embedded as a tab page (Full UI tab mode).
  w_->pipeline_window_->setWindowFlag(Qt::Window, true);
  w_->pipeline_window_->refresh();
  w_->pipeline_window_->show();
  w_->pipeline_window_->raise();
  w_->pipeline_window_->activateWindow();
}

void SettingsController::show_instance_switcher() {
  auto instances_dir = engine::default_instances_dir();

  InstanceSwitcherDialog dlg(w_->plugin_loader_, w_);
  dlg.load_instances(instances_dir.string());

  if (dlg.exec() != QDialog::Accepted)
    return;

  if (dlg.create_requested()) {
    create_new_instance();
    return;
  }

  auto selected = dlg.selected_instance().toStdString();
  if (selected.empty())
    return;

  switch_to_instance(QString::fromStdString(selected));
}

bool SettingsController::create_new_instance() {
  auto instances_dir = engine::default_instances_dir();

  std::vector<engine::DetectedGame> installed_games;
  if (w_->plugin_loader_) {
    std::vector<std::pair<uint32_t, std::pair<std::string, std::string>>>
        game_specs;
    for (const auto &p : w_->plugin_loader_->game_plugins()) {
      if (p.steam_appid > 0)
        game_specs.push_back(
            {p.steam_appid, {p.game_id, p.game_display_name}});
    }
    installed_games =
        engine::GameDetector::detect_steam_games_multi(game_specs);
  }

  std::vector<ui::GameEntry> installed_entries, available_entries;
  for (const auto &g : installed_games) {
    ui::GameEntry e;
    e.game_id = g.game_id;
    e.display_name = g.name;
    e.steam_appid = g.steam_appid;
    e.installed = true;
    e.install_path = g.install_path;
    installed_entries.push_back(e);
  }
  if (w_->plugin_loader_) {
    for (const auto &p : w_->plugin_loader_->game_plugins()) {
      bool found = false;
      for (const auto &ie : installed_entries) {
        if (ie.game_id == p.game_id) {
          found = true;
          break;
        }
      }
      if (!found) {
        ui::GameEntry e;
        e.game_id = p.game_id;
        e.display_name = p.game_display_name;
        e.steam_appid = p.steam_appid;
        e.installed = false;
        available_entries.push_back(e);
      }
    }
  }

  QDialog sel_dlg(w_);
  sel_dlg.setWindowTitle(tr("Create New Instance"));
  sel_dlg.setMinimumSize(600, 400);
  auto *layout = new QVBoxLayout(&sel_dlg);
  auto *selection = new ui::GameSelectionWidget(&sel_dlg);
  selection->set_games(installed_entries, available_entries);
  layout->addWidget(selection);

  ui::GameEntry chosen;
  bool chosen_ok = false;
  QObject::connect(selection, &ui::GameSelectionWidget::game_selected,
                   [&](const ui::GameEntry &entry) {
                     chosen = entry;
                     chosen_ok = true;
                     sel_dlg.accept();
                   });

  if (sel_dlg.exec() != QDialog::Accepted || !chosen_ok)
    return false;

  engine::DetectedGame dg;
  dg.game_id = chosen.game_id;
  dg.name = chosen.display_name;
  dg.steam_appid = chosen.steam_appid;
  dg.install_path = chosen.install_path;

  auto inst = engine::create_instance_for_game(dg, instances_dir);
  if (inst.info().game_id.empty()) {
    QMessageBox::warning(
        w_, tr("Error"),
        tr("Failed to create instance for %1")
            .arg(QString::fromStdString(chosen.display_name)));
    return false;
  }

  engine::write_last_instance(inst.info().root.filename().string());
  set_game_info(chosen.game_id, chosen.display_name, "Default",
                chosen.install_path, inst.info().root);
  engine::Logger::instance().debug("Created and switched to instance: " +
                                   inst.info().root.filename().string());
  return true;
}

bool SettingsController::switch_to_instance(const QString &name) {
  auto instances_dir = engine::default_instances_dir();
  auto selected = name.toStdString();
  if (selected.empty())
    return false;

  // Don't reload if already on w_ instance
  if (!w_->current_instance_root_.empty() &&
      instances_dir / selected == w_->current_instance_root_)
    return true;

  auto inst = engine::Instance::installed(selected, instances_dir);
  if (!inst.read_toml()) {
    QMessageBox::warning(w_, tr("Error"),
                         tr("Failed to read instance.toml for %1").arg(name));
    return false;
  }

  auto &info = inst.info();
  std::string game_id = info.game_id;
  std::string display_name;
  if (w_->plugin_loader_) {
    display_name = w_->plugin_loader_->display_name_for(game_id);
  }
  if (display_name.empty())
    display_name = game_id;

  engine::write_last_instance(selected);
  set_game_info(game_id, display_name, "Default", info.game_dir,
                inst.info().root);
  engine::Logger::instance().debug("Switched to instance: " + selected);
  return true;
}

void SettingsController::refresh_recent_instances() {
  if (!w_->menu_bar_)
    return;
  auto instances_dir = engine::default_instances_dir();
  std::vector<std::string> names;
  std::error_code ec;
  if (std::filesystem::is_directory(instances_dir, ec)) {
    for (const auto &entry :
         std::filesystem::directory_iterator(instances_dir, ec)) {
      if (!entry.is_directory())
        continue;
      if (!std::filesystem::exists(entry.path() / "instance.toml"))
        continue;
      names.push_back(entry.path().filename().string());
    }
  }
  std::sort(names.begin(), names.end());
  w_->menu_bar_->set_recent_instances(names);
}

bool SettingsController::handle_global_event(QObject *obj, QEvent *event) {
  if (event->type() == QEvent::KeyPress &&
      !w_->current_instance_root_.empty()) {
    auto *ke = static_cast<QKeyEvent *>(event);
    int key = ke->key();
    (void)obj;

    if (w_->konami_state_ < 10) {
      if (key == w_->konami_sequence_[w_->konami_state_]) {
        ++w_->konami_state_;
      } else if (key == w_->konami_sequence_[0]) {
        w_->konami_state_ = 1;
      } else {
        w_->konami_state_ = 0;
      }
    }

    if (w_->konami_state_ == 10 &&
        (key == Qt::Key_Enter || key == Qt::Key_Return)) {
      auto flag = w_->current_instance_root_ / "debugging.enabled";
      std::error_code ec;
      if (!std::filesystem::exists(flag, ec)) {
        std::ofstream ofs(flag.string());
        ofs << "enabled\n";
        engine::Logger::instance().debug(
            "Debug mode enabled (Konami code entered)");
        w_->status_bar_->set_status(tr("Debug mode enabled"));
      } else {
        std::filesystem::remove(flag, ec);
        engine::Logger::instance().debug("Debug mode disabled");
        w_->status_bar_->set_status(tr("Debug mode disabled"));
      }
      w_->konami_state_ = 0;

      bool enabled = std::filesystem::exists(flag);
      if (enabled) {
        if (!w_->debug_window_) {
          w_->debug_window_ = new DebugWindow(
              w_->current_instance_root_, w_->current_game_id_,
              w_->current_game_name_, w_->plugin_loader_,
              [this]() {
                if (w_->style_manager_)
                  w_->style_manager_->reload_current();
              },
              w_);
        }
        w_->debug_window_->show();
        w_->debug_window_->raise();
      } else if (w_->debug_window_) {
        w_->debug_window_->hide();
      }
      return true;
    }
  }
  return false;
}

} // namespace ui

#include "moc_settings_controller.cpp"
