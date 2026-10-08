#pragma once

#include "engine/log/logger.h"

#include <QFrame>
#include <QPoint>
#include <QString>

class QEvent;
class QPlainTextEdit;

namespace engine {
class Logger;
}

namespace ui {

class ConsolePanel : public QFrame {
  Q_OBJECT
public:
  // Newest lines kept on screen; older ones are dropped from the top. The log
  // file itself is never truncated, only the view.
  static constexpr int kMaxLines = 1000;

  explicit ConsolePanel(QWidget *parent = nullptr);

  void append_text(const QString &text);
  void append_log(const QString &tag, const QString &timestamp, const QString &message,
                  int level);
  void clear();

  // Lowest level the view shows. Lines below it are dropped on arrival and
  // never make it into the document, so raising the verbosity cannot bring
  // them back from the document - but Logger::replayed() still has them, which
  // is what set_min_level re-renders from.
  [[nodiscard]] engine::LogLevel min_level() const { return min_level_; }
  // Re-filter the whole view at `level` and persist it to diagnostics/log_level,
  // so the choice survives a restart like every other verbosity setting. Also
  // moves Logger's floor, so a level picked here is logged from now on instead
  // of being dropped at the source.
  void set_min_level(engine::LogLevel level);

private:
  // Right-click menu (MO2 LogList::createContextMenu, loglist.cpp:250-290):
  // Copy, Copy all, Clear, Open logs folder, and a level submenu.
  void on_context_menu(const QPoint &pos);
  void on_open_logs_folder();
  // "DBG" / "INF" / "WRN" / "ERR", shared by the live path and the re-render.
  [[nodiscard]] static QString level_tag(int level);

  QPlainTextEdit *output_ = nullptr;
  // Read from the Logger callback, which hops to this thread before touching
  // it, so no synchronisation is needed.
  engine::LogLevel min_level_ = engine::LogLevel::Info;
};

}  // namespace ui