#include "ui/controllers/queue_controller.h"
#include "ui/controllers/downloads_controller.h"
#include "ui/controllers/mod_list_controller.h"

#include <QLabel>

#include "engine/game/detect/mod_scanner.h"
#include "engine/core/events/event_bus.h"
#include "engine/core/log/logger.h"
#include "engine/source/nxm/nxm_router.h"
#include "engine/game/registry/game_knowledge.h"
#include "ui/main_window/main_window.h"

namespace ui {

QueueController::QueueController(MainWindow *w, QObject *parent)
    : QObject(parent), w_(w) {}

void QueueController::flush_pending_changes() {
  if (w_->pending_changes_.empty())
    return;
  if (!w_->knowledge_ || w_->current_game_id_.empty() ||
      w_->current_game_dir_.empty())
    return;
  if (!w_->mod_model_)
    return;

  engine::Logger::instance().debug("Flushing " +
                                   std::to_string(w_->pending_changes_.size()) +
                                   " queued mod changes");

  auto mods_subpath =
      w_->knowledge_->get(w_->current_game_id_, "mods_subpath", "");
  if (mods_subpath.empty()) {
    engine::Logger::instance().warn(
        "Cannot flush changes: mods_subpath is empty");
    w_->pending_changes_.clear();
    return;
  }

  // Apply toggles (latest state per mod wins - already deduplicated by
  // sync_mod_enable_state)
  for (const auto &pt : w_->pending_changes_) {
    auto mod_folder =
        w_->resolve_mod_folder(pt.mod_id.toStdString(), mods_subpath);
    if (pt.enabled) {
      (void)engine::ModScanner::enable_mod(*w_->knowledge_,
                                           w_->current_game_id_, mod_folder);
    } else {
      (void)engine::ModScanner::disable_mod(*w_->knowledge_,
                                            w_->current_game_id_, mod_folder);
    }
    // P1.3 event bus: mirror MO2 onModStateChanged for the deferred
    // (game-running) toggle path — the state only actually changed on disk
    // here, so this is the moment to emit, not at queue time.
    engine::EventBus::instance().dispatch(
        engine::events::kModStateChanged,
        engine::json_obj({
            {"mod", pt.mod_id.toStdString()},
            {"enabled", pt.enabled ? "1" : "0"},
        }));
  }

  // Save final mod order (priorities may have changed via drag-drop while game
  // ran)
  auto saved_pid = w_->running_process_pid_;
  w_->running_process_pid_ = -1; // bypass game-running guard in sync_priorities
  w_->mod_list_->sync_priorities();
  w_->running_process_pid_ = saved_pid;

  w_->pending_changes_.clear();
  if (w_->pending_queue_label_)
    w_->pending_queue_label_->hide();
  engine::Logger::instance().debug("Queued mod changes flushed");
}

void QueueController::update_queue_label() {
  if (!w_->pending_queue_label_)
    return;
  if (w_->pending_changes_.empty()) {
    w_->pending_queue_label_->hide();
    return;
  }
  w_->pending_queue_label_->setText(
      tr("Changes queued: %1 (apply on game exit)")
          .arg(w_->pending_changes_.size()));
  w_->pending_queue_label_->show();
}

void QueueController::flush_pending_nxm() {
  if (w_->pending_nxm_url_.empty())
    return;

  auto link = engine::NxmRouter::parse(w_->pending_nxm_url_);
  w_->pending_nxm_url_.clear();

  if (link.valid()) {
    w_->downloads_->handle_nxm_download(link);
  }
}

} // namespace ui