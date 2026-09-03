#include "ui/controllers/downloads_controller.h"
#include "platform/platform.h"
#include "ui/controllers/mod_list_controller.h"
#include "ui/fomod/fomod_wizard_dialog.h"
#include "ui/install/install_name_dialog.h"
#include "ui/overwrite/query_overwrite_dialog.h"

#include <QAbstractButton>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <vector>

#include "engine/core/events/event_bus.h"
#include "engine/core/log/logger.h"
#include "engine/core/trace/trace_recorder.h"
#include "engine/core/util/fs_utils.h"
#include "engine/deploy/interface.h"
#include "engine/deploy/launch/overlay_launcher.h"
#include "engine/game/registry/game_knowledge.h"
#include "engine/pipeline/extract_stage.h"
#include "engine/pipeline/fomod_stage.h"
#include "engine/pipeline/install_stage.h"
#include "engine/pipeline/plugin_claim_stage.h"
#include "engine/pipeline/plugin_host/plugin_loader.h"
#include "engine/pipeline/registry/stage_registry.h"
#include "engine/source/loverslab_auth.h"
#include "engine/source/loverslab_provider.h"
#include "engine/source/nexus_provider.h"
#include "engine/source/nxm/managed_games.h"
#include "engine/source/nxm/nxm_router.h"
#include "engine/source/steam_workshop_provider.h"
#include "ui/install/install_progress_dialog.h"
#include "ui/main_window/main_window.h"
#include "ui/nxm/nxm_ipc.h"
#include "ui/panels/tab_panels.h"
#include "ui/settings/settings.h"
#include "ui/widgets/right_panel.h"
#include "ui/workers/pipeline_worker.h"
#include "ui/network/network_options_bridge.h"

namespace ui {

DownloadsController::DownloadsController(MainWindow *w, QObject *parent)
    : QObject(parent), w_(w) {}

void DownloadsController::setup_pipeline() {
  w_->pipeline_thread_ = new PipelineThread(w_);
  w_->pipeline_thread_->start();
  // Nexus downloads queue one-at-a-time per Settings (free Regular/Supporter
  // accounts are throttled; see Settings::nexus_queue_downloads). Pushed at
  // startup and re-pushed after the settings dialog closes.
  w_->pipeline_thread_->worker()->set_nexus_queue_downloads(
      Settings::instance().nexus_queue_downloads());
  // Push the network options once at startup so any startup-time fetch
  // (masterlists, update check, icon cache) sees the current Settings.
  engine::network::push_settings_to_network();

  // Download finished (download-only, MO2 model): the row becomes Complete
  // with a real file path; installation is a separate user-triggered step.
  connect(w_->pipeline_thread_->worker(), &PipelineWorker::download_complete,
          this,
          [this](const std::string &id, bool success,
                 const std::string &archive_path, const std::string &name) {
            auto *dt = w_->right_panel_->downloads_tab();
            if (dt) {
              if (!archive_path.empty())
                dt->set_file_path(id, archive_path);
              // Replace the "Mod #<id> - file <id>" placeholder with the real
              // name the provider resolved (empty = nothing available).
              if (!name.empty())
                dt->rename_download(id, name);
              dt->mark_complete(id, success);
            }
            // Persist download state
            save_download_manifest();
          });

  // Download metadata resolved by the provider right before the bytes flow:
  // replace the "Mod #<id> - file <id>" / "LoversLab file <id>" placeholder
  // with the real name immediately, instead of only when the download ends.
  // Prefer the source-resolved mod/file name; fall back to the raw archive
  // name (extension stripped) so the row is descriptive either way.
  connect(w_->pipeline_thread_->worker(), &PipelineWorker::download_meta, this,
          [this](const std::string &id, const std::string &archive_name,
                 const std::string &display_name) {
            auto *dt = w_->right_panel_->downloads_tab();
            if (!dt)
              return;
            std::string name = display_name;
            if (name.empty() && !archive_name.empty())
              name = std::filesystem::path(archive_name).stem().string();
            if (!name.empty())
              dt->rename_download(id, name);
          });

  // Install finished (user-triggered via the Downloads context menu or
  // double-click): add just the newly installed row to the mod list instead
  // of reloading the whole mods dir, and mark the entry Installed. The UI
  // lock put up at install_requested is released on every terminal signal
  // (success, failure, cancel). A failed install leaves the download row in
  // its download state - only a failed download marks it Failed.
  connect(
      w_->pipeline_thread_->worker(), &PipelineWorker::install_complete, this,
      [this](const std::string &mod_id, bool success, const std::string &,
             const std::string &installed_folder) {
        hide_install_progress();
        w_->set_ui_enabled(true);
        auto *dt = w_->right_panel_->downloads_tab();
        if (dt) {
          if (success) {
            if (!w_->current_game_id_.empty() && !installed_folder.empty()) {
              engine::Logger::instance().debug("Install finished for " +
                                               mod_id + ", adding " +
                                               installed_folder);
              w_->mod_list_->add_installed_mod(installed_folder);
              // P1.3 event bus: mirror MO2 onModInstalled. The bus
              // handler runs synchronously here (install is UI-thread);
              // a subscribed plugin must not block.
              engine::EventBus::instance().dispatch(
                  engine::events::kModInstalled, engine::json_obj({
                                                     {"mod", installed_folder},
                                                     {"name", installed_folder},
                                                 }));
            }
            dt->mark_installed(mod_id);
          }
          // Install failure is NOT a download failure: the row keeps its
          // download state (Complete, retryable Install button). The error
          // was already logged to the console by the pipeline worker /
          // extract stage - don't flip the download to Failed here.
        }
        // Persist download state
        save_download_manifest();
      });

  // Install canceled by the user (FOMOD wizard or overwrite dialog): NOT a
  // failure - leave the download in whatever state it had (no Failed mark).
  // Release the UI lock the same way as install_complete.
  connect(w_->pipeline_thread_->worker(), &PipelineWorker::install_canceled,
          this, [this](const std::string &) {
            hide_install_progress();
            w_->set_ui_enabled(true);
            save_download_manifest();
          });

  // Forward install-stage progress (extract/copy) to the install progress
  // popup. Emitted on the worker thread; this connection auto-queues it.
  connect(w_->pipeline_thread_->worker(), &PipelineWorker::install_progress,
          this, &DownloadsController::update_install_progress);

  // A download was paused mid-fetch (partial file kept for resume).
  connect(w_->pipeline_thread_->worker(), &PipelineWorker::paused, this,
          [this](const std::string &id) {
            auto *dt = w_->right_panel_->downloads_tab();
            if (dt)
              dt->mark_paused(id);
            save_download_manifest();
          });

  // Forward download progress to the DownloadsTab
  connect(w_->pipeline_thread_->worker(), &PipelineWorker::download_progress,
          this,
          [this](const std::string &mod_id, int64_t dl, int64_t total,
                 double speed) {
            auto *dt = w_->right_panel_->downloads_tab();
            if (dt)
              dt->update_progress(mod_id, dl, total, speed);
          });

  // Register built-in source providers
  engine::SourceRegistry::instance().register_provider(
      std::make_unique<engine::Source::Nexus::Provider>());
  engine::SourceRegistry::instance().register_provider(
      std::make_unique<engine::LoversLabProvider>());

  std::string ws_db = engine::safe_home_dir().string() +
                      "/.local/share/GameModManager/workshop_cache.db";
  engine::SourceRegistry::instance().register_provider(
      std::make_unique<engine::SteamWorkshopProvider>(
          ws_db, Settings::instance().workshop_rate_limit_per_hour()));
}

void DownloadsController::setup_nxm_ipc() {
  // Start IPC server to receive nxm:// URLs from other GMM processes
  w_->nxm_ipc_ = new engine::NxmIpcServer(w_);
  if (w_->nxm_ipc_->startListening()) {
    connect(w_->nxm_ipc_, &engine::NxmIpcServer::nxmUrlReceived, this,
            [this](const QString &url) {
              std::string raw = url.toStdString();
              // Accept gmm:// URLs too - convert to nxm:// for the parser
              static const std::string gmm_pre = "gmm://nexus/";
              if (raw.compare(0, gmm_pre.size(), gmm_pre) == 0)
                raw = "nxm://" + raw.substr(gmm_pre.size());
              auto link = engine::NxmRouter::parse(raw);
              if (link.valid()) {
                handle_nxm_download(link);
              }
            });
  }
}

bool DownloadsController::confirm_close() {
  // Closing cancels in-flight downloads. Warn first unless the user told us
  // to never ask again ("Don't Ask" persists the preference). MO2 asks the
  // same question (mainwindow.cpp canExit); here Ok/Quit/Don't Ask all
  // proceed with the close, Cancel aborts it.
  auto *dt = w_->right_panel_ ? w_->right_panel_->downloads_tab() : nullptr;
  if (dt && dt->has_active_download() &&
      Settings::instance().confirm_close_with_downloads()) {
    QMessageBox box(w_);
    box.setWindowTitle(tr("Active Downloads"));
    box.setIcon(QMessageBox::Warning);
    box.setText(tr("You have active downloads in progress.\n"
                   "Closing the application will cancel them."));
    auto *ok_btn = box.addButton(tr("Ok"), QMessageBox::AcceptRole);
    auto *quit_btn = box.addButton(tr("Quit"), QMessageBox::AcceptRole);
    auto *dont_ask_btn =
        box.addButton(tr("Don't Ask"), QMessageBox::AcceptRole);
    auto *cancel_btn = box.addButton(tr("Cancel"), QMessageBox::RejectRole);
    box.setDefaultButton(cancel_btn);
    box.exec();

    QAbstractButton *clicked = box.clickedButton();
    if (clicked == cancel_btn) {
      return false;
    }
    if (clicked == dont_ask_btn) {
      Settings::instance().set_confirm_close_with_downloads(false);
    }
    // Ok / Quit / Don't Ask all fall through to the close.
  }
  return true;
}

void DownloadsController::save_download_manifest() {
  auto *dt = w_->right_panel_->downloads_tab();
  if (!dt)
    return;
  auto path = w_->download_manifest_path();
  if (path.empty())
    return;
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec)
    return;
  std::ofstream out(path);
  if (!out)
    return;
  out << dt->serialize();
}

void DownloadsController::load_download_manifest() {
  auto path = w_->download_manifest_path();
  if (path.empty() || !std::filesystem::exists(path))
    return;
  std::ifstream in(path);
  if (!in)
    return;
  std::string json((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());
  if (json.empty())
    return;
  auto *dt = w_->right_panel_->downloads_tab();
  if (!dt)
    return;
  auto downloads_dir = w_->downloads_dir_path();
  dt->deserialize(json, downloads_dir);
}

void DownloadsController::wire_downloads_tab() {
  auto *dt = w_->right_panel_->downloads_tab();
  if (!dt)
    return;

  // Manifest first, then the dir scan: deserialize populates pipeline
  // entries (ids like "<mod_id>-<file_id>") before the scan, so their
  // archives are recognized as already-tracked instead of duplicated as
  // "Manual" rows.
  load_download_manifest();
  dt->set_downloads_dir(w_->downloads_dir_path());

  connect(dt, &DownloadsTab::install_requested, this,
          [this](const std::string &mod_id, const std::filesystem::path &fp,
                 const std::string &source_type, const std::string &source_id,
                 int file_id, const std::string &display_name,
                 const std::string &page_url) {
            if (!w_->pipeline_thread_)
              return;
            // Lock the interface for the duration of the install so the user
            // can't race it (re-trigger, edit the mod list, quit mid-copy).
            // Released by install_complete / install_canceled.
            w_->set_ui_enabled(false);
            QMetaObject::invokeMethod(
                w_->pipeline_thread_->worker(),
                [this, mod_id, fp, source_type, source_id, file_id,
                 display_name, page_url]() {
                  w_->pipeline_thread_->worker()->install_mod(
                      mod_id, fp.string(), source_type, source_id, file_id,
                      display_name, page_url);
                },
                Qt::QueuedConnection);
          });
  connect(dt, &DownloadsTab::loverslab_url_entered, this,
          &DownloadsController::start_loverslab_download);
  connect(dt, &DownloadsTab::pause_requested, this,
          [this](const std::string &id) {
            if (!w_->pipeline_thread_)
              return;
            QMetaObject::invokeMethod(
                w_->pipeline_thread_->worker(),
                [this, id]() {
                  w_->pipeline_thread_->worker()->pause_download(id);
                },
                Qt::QueuedConnection);
          });
  connect(
      dt, &DownloadsTab::resume_requested, this, [this](const std::string &id) {
        if (!w_->pipeline_thread_)
          return;
        auto *dtab = w_->right_panel_->downloads_tab();
        if (dtab)
          dtab->mark_downloading(id);
        auto mods_dir = w_->mods_dir_path().string();
        auto meta_dir = w_->current_instance_root_.empty()
                            ? ""
                            : (w_->current_instance_root_ / "meta").string();
        // Nexus downloads resume with their original NXM link...
        auto it_nxm = w_->nxm_links_.find(id);
        if (it_nxm != w_->nxm_links_.end()) {
          auto link = it_nxm->second;
          QMetaObject::invokeMethod(
              w_->pipeline_thread_->worker(),
              [this, id, link, mods_dir, meta_dir]() {
                w_->pipeline_thread_->worker()->download_mod(
                    id, link, w_->current_game_id_, mods_dir, meta_dir);
              },
              Qt::QueuedConnection);
          return;
        }
        // ...LoversLab downloads resume with their original URL.
        auto it_url = w_->url_downloads_.find(id);
        if (it_url != w_->url_downloads_.end()) {
          auto url = it_url->second;
          QMetaObject::invokeMethod(
              w_->pipeline_thread_->worker(),
              [this, id, url, mods_dir, meta_dir]() {
                w_->pipeline_thread_->worker()->download_mod_url(
                    id, url, w_->current_game_id_, mods_dir, meta_dir);
              },
              Qt::QueuedConnection);
        }
      });
  connect(dt, &DownloadsTab::entry_removed, this,
          [this](const std::string &id) {
            w_->nxm_links_.erase(id);
            w_->url_downloads_.erase(id);
            save_download_manifest();
          });
}

void DownloadsController::wire_saves_tab() {
  auto *st = w_->right_panel_->saves_tab();
  if (!st)
    return;

  // Saves live under documents/My Games/<game> (game_mygames_dir()). The
  // "Saves" leaf is knowledge-driven where the game plugin defines it; the
  // Bethesda-family default applies to Skyrim/FO4/etc.
  auto sub =
      w_->knowledge_->get(w_->current_game_id_, "saves_subpath", "Saves");
  const auto saves_dir = w_->game_mygames_dir() / sub;
  engine::Logger::instance().debug("Saves tab: scanning " + saves_dir.string());
  st->set_saves_dir(saves_dir);

  // No directory watcher and no mod-list/plugin-driven rescans: the Saves
  // dir is scanned exactly once here (at load, when the tab is wired) and
  // after a delete. Earlier versions re-scanned on every mod_list_changed
  // (via refresh_plugins_tab) to keep the missing-asset column in sync, but
  // that made a separator fold/unfold trigger a full save scan - so the
  // missing-asset column now reflects launch-time load order only.
  connect(st, &ui::SavesTab::delete_requested, this,
          &DownloadsController::on_saves_delete_requested);

  // Initial fill (the delete flow triggers the only later scan).
  on_saves_refresh_requested();
}

void DownloadsController::on_saves_refresh_requested() {
  auto *st = w_->right_panel_->saves_tab();
  if (!st)
    return;

  ui::SavesScanRequest request;
  request.saves_dir = st->saves_dir();
  if (request.saves_dir.empty())
    return;
  request.extensions = {"ess"};
  request.game_id = w_->current_game_id_;
  // Snapshot the plugin list so results reflect the load order at the moment
  // the refresh was asked for (missing-asset state moves with toggles).
  request.plugins = w_->plugins_db_.plugins();
  request.mods_dir = w_->mods_dir_path();
  request.overwrite_dir = w_->overwrite_dir_path();
  st->request_scan(std::move(request));
}

void DownloadsController::on_saves_delete_requested(
    const QStringList &filepaths) {
  // Trash (never permanent): engine::remove_path -> QDir::moveToTrash.
  for (const auto &fp : filepaths) {
    engine::remove_path(std::filesystem::path(fp.toStdString()),
                        /*permanent=*/false);
  }
  // The saved state no longer matches disk; re-scan.
  on_saves_refresh_requested();
}

void DownloadsController::update_install_progress(const std::string &mod_id,
                                                  int percent,
                                                  const std::string &status) {
  // A new install resets the popup (title, bar) and (re)arms the deferred
  // show. Subsequent updates for the same install just refresh it.
  if (mod_id != w_->active_install_progress_id_) {
    w_->active_install_progress_id_ = mod_id;
    if (!w_->install_progress_dialog_) {
      w_->install_progress_dialog_ = new ui::InstallProgressDialog(w_);
    }
    w_->install_progress_dialog_->begin(tr("Installing…"));
    if (!w_->install_progress_show_timer_) {
      w_->install_progress_show_timer_ = new QTimer(w_);
      w_->install_progress_show_timer_->setSingleShot(true);
      connect(w_->install_progress_show_timer_, &QTimer::timeout, this,
              [this]() {
                if (w_->install_progress_dialog_)
                  w_->install_progress_dialog_->show();
              });
    }
    // ~300ms delay so a quick install never flashes the dialog (MO2
    // behaves the same way - the popup only appears for the slow part).
    w_->install_progress_show_timer_->start(300);
  } else if (w_->install_progress_dialog_ &&
             !w_->install_progress_dialog_->isVisible()) {
    // The dialog was hidden by an interactive install dialog (FOMOD wizard,
    // name confirm, overwrite) - it is back to being informative now, so
    // show it immediately (the 300ms delay already elapsed long ago).
    w_->install_progress_dialog_->show();
  }

  if (w_->install_progress_dialog_) {
    w_->install_progress_dialog_->set_status(QString::fromStdString(status),
                                             percent);
  }
}

void DownloadsController::hide_install_progress() {
  w_->active_install_progress_id_.clear();
  if (w_->install_progress_show_timer_)
    w_->install_progress_show_timer_->stop();
  if (w_->install_progress_dialog_)
    w_->install_progress_dialog_->hide();
}

void DownloadsController::handle_nxm_download(const engine::NxmLink &link) {
  if (!link.valid()) {
    engine::Logger::instance().warn("Invalid NXM link received");
    return;
  }

  // Redact the signed download key in the log - it is a bearer token. The
  // rest of the URL (expires, user_id, param names) stays visible so a
  // session can verify whether the browser delivered the query string.
  std::string log_url = link.full_url;
  {
    auto kp = log_url.find("key=");
    if (kp != std::string::npos) {
      auto ke = log_url.find_first_of("&", kp);
      log_url = log_url.substr(0, kp + 4) +
                (ke != std::string::npos ? log_url.substr(ke) : "");
    }
  }

  engine::Logger::instance().debug(
      "NXM download: domain=" + link.nexus_domain +
      " mod=" + std::to_string(link.mod_id) +
      " file=" + std::to_string(link.file_id) + " key=" +
      (link.key.empty() ? "absent"
                        : "present(" + std::to_string(link.key.size()) + "B)") +
      " expires=" + (link.expire > 0 ? std::to_string(link.expire) : "none") +
      " url=" + log_url);

  // Find which game_id owns this nexus_domain via managed games
  std::string matched_game_id;
  if (w_->managed_games_) {
    matched_game_id = w_->managed_games_->game_id_for_domain(link.nexus_domain);
  }

  // Fallback: try matching via loaded plugins
  if (matched_game_id.empty() && w_->plugin_loader_) {
    for (const auto &p : w_->plugin_loader_->plugins()) {
      if (p.nexus_domain == link.nexus_domain) {
        matched_game_id = p.game_id;
        break;
      }
    }
  }

  if (matched_game_id.empty()) {
    QMessageBox::warning(w_, tr("NXM Download"),
                         tr("Unknown Nexus Mods domain: %1\nNo game plugin "
                            "supports this domain.")
                             .arg(QString::fromStdString(link.nexus_domain)));
    return;
  }

  // Is this game managed by us?
  bool is_managed =
      w_->managed_games_ && w_->managed_games_->is_managed(matched_game_id);

  if (!is_managed) {
    QMessageBox::information(
        w_, tr("NXM Download"),
        tr("This mod is for %1, but GameModManager is not managing this "
           "game.\n\n"
           "Open the game's instance first to register it, then try the link "
           "again.")
            .arg(QString::fromStdString(
                w_->plugin_loader_->display_name_for(matched_game_id))));
    return;
  }

  // Game is managed - but is the active instance the right one?
  if (matched_game_id != w_->current_game_id_) {
    QMessageBox::information(
        w_, tr("NXM Download"),
        tr("This mod is for %1, but the active instance is %2.\n"
           "Switch to the correct instance first.")
            .arg(QString::fromStdString(
                w_->plugin_loader_->display_name_for(matched_game_id)))
            .arg(QString::fromStdString(w_->current_game_name_)));
    return;
  }

  // Route to the active instance - start download via pipeline worker
  engine::Logger::instance().debug(
      "Starting download: " + w_->current_game_name_ +
      " (mod_id=" + std::to_string(link.mod_id) +
      ", file_id=" + std::to_string(link.file_id) + ")");

  // Show in DownloadsTab immediately. The entry key is "<mod_id>-<file_id>"
  // so Main and Optional files of the same mod page stay separate entries.
  const auto mod_id = std::to_string(link.mod_id);
  const auto file_id = std::to_string(link.file_id);
  const auto key = mod_id + "-" + file_id;

  auto *dt = w_->right_panel_->downloads_tab();
  if (dt) {
    dt->add_download(key,
                     tr("Mod #%1 - file %2")
                         .arg(QString::fromStdString(mod_id))
                         .arg(link.file_id)
                         .toStdString(),
                     "Nexus Mods", {}, link.nexus_domain, link.file_id, mod_id);
  }

  // Surface the download: bring the window to front and switch to the
  // Downloads tab so the user sees the new entry start.
  if (w_->isMinimized()) {
    w_->showNormal();
  }
  w_->raise();
  w_->activateWindow();
  w_->right_panel_->show_downloads_tab();

  // Keep the NXM link so a paused download can be resumed later.
  w_->nxm_links_[key] = link;

  // Build paths for the pipeline context
  auto mods_dir = w_->mods_dir_path();
  auto meta_dir = w_->current_instance_root_.empty()
                      ? ""
                      : (w_->current_instance_root_ / "meta").string();

  // Invoke the pipeline worker asynchronously (download only - install is a
  // separate user-triggered step)
  QMetaObject::invokeMethod(
      w_->pipeline_thread_->worker(),
      [this, key, link, mods_dir, meta_dir]() {
        w_->pipeline_thread_->worker()->download_mod(
            key, link, w_->current_game_id_, mods_dir.string(), meta_dir);
      },
      Qt::QueuedConnection);

  engine::Logger::instance().debug("Download queued for mod " + mod_id +
                                   " file " + file_id);
}

void DownloadsController::start_loverslab_download(const std::string &url) {
  if (!engine::LoversLabProvider::is_loverslab_url(url)) {
    QMessageBox::warning(
        w_, tr("Download from URL"),
        tr("Not a LoversLab download link.\n\n"
           "Right-click a file's Download button on loverslab.com and copy "
           "the link address (it ends in ?do=download), then paste it here."));
    return;
  }

  if (!engine::LoversLabAuth::instance().has_cookie()) {
    QMessageBox::information(
        w_, tr("Download from URL"),
        tr("No LoversLab session cookie is configured.\n\n"
           "LoversLab has no public API, so downloads need the session "
           "cookie from a signed-in browser tab. Set it under Settings > "
           "Sources > LoversLab, then try again."));
    return;
  }

  // Redact the CSRF token in logs - it is a session-bound secret.
  std::string log_url = url;
  {
    auto kp = log_url.find("csrfKey=");
    if (kp != std::string::npos) {
      auto ke = log_url.find_first_of("&", kp);
      log_url = log_url.substr(0, kp + 8) +
                (ke != std::string::npos ? log_url.substr(ke) : "");
    }
  }
  engine::Logger::instance().debug("LoversLab download: " + log_url);

  // Entry key is the file id when the URL carries one, else a stable hash
  // (keeps map keys and archive-name fallbacks free of '/' characters).
  std::string file_id = engine::LoversLabProvider::extract_file_id(url);
  const std::string key =
      file_id.empty() ? "ll-" + std::to_string(std::hash<std::string>{}(url))
                      : file_id;

  auto *dt = w_->right_panel_->downloads_tab();
  if (dt) {
    dt->add_download(
        key,
        tr("LoversLab file %1").arg(QString::fromStdString(key)).toStdString(),
        "LoversLab", {}, {}, 0, {},
        engine::LoversLabProvider::mod_page_url(url));
  }

  // Surface the download: bring the window to front and switch to the
  // Downloads tab so the user sees the new entry start.
  if (w_->isMinimized()) {
    w_->showNormal();
  }
  w_->raise();
  w_->activateWindow();
  w_->right_panel_->show_downloads_tab();

  // Keep the URL so a paused download can be resumed later.
  w_->url_downloads_[key] = url;

  // Build paths for the pipeline context
  auto mods_dir = w_->mods_dir_path();
  auto meta_dir = w_->current_instance_root_.empty()
                      ? ""
                      : (w_->current_instance_root_ / "meta").string();

  QMetaObject::invokeMethod(
      w_->pipeline_thread_->worker(),
      [this, key, url, mods_dir, meta_dir]() {
        w_->pipeline_thread_->worker()->download_mod_url(
            key, url, w_->current_game_id_, mods_dir.string(), meta_dir);
      },
      Qt::QueuedConnection);

  engine::Logger::instance().debug("LoversLab download queued: " + key);
}

} // namespace ui