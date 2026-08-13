#pragma once

#include <QObject>
#include <QString>

#include "ui/main_window/main_window.h"

namespace ui {

// Deferred-until-game-exit queues: pending mod toggles (applied when the game
// exits) and pending NXM downloads (flushed after the game closes). Split out
// of the 7211-line main_window.cpp (Issue #16).
class QueueController : public QObject {
  Q_OBJECT
public:
  explicit QueueController(MainWindow *w, QObject *parent = nullptr);

public slots:
  void flush_pending_changes();
  void update_queue_label();
  void flush_pending_nxm();

private:
  MainWindow *w_ = nullptr;
};

} // namespace ui