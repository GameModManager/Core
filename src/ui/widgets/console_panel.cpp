#include "ui/widgets/console_panel.h"
#include "ui/settings/settings.h"
#include "engine/util/debug_env.h"
#include "engine/log/logger.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QEvent>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPointer>
#include <QScrollBar>
#include <QShortcut>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QUrl>
#include <QVBoxLayout>

#include <cstdlib>

namespace ui {

ConsolePanel::ConsolePanel(QWidget *parent) : QFrame(parent) {
  setFrameShape(QFrame::StyledPanel);
  setFrameShadow(QFrame::Sunken);

  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);

  output_ = new QPlainTextEdit(this);
  output_->setReadOnly(true);
  output_->setUndoRedoEnabled(false);
  output_->setFont(QFont("Monospace", 9));
  output_->setFocusPolicy(Qt::StrongFocus);
  output_->setLineWrapMode(QPlainTextEdit::NoWrap);
  output_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
  // Ring the buffer: a session that launches a game and installs mods logs
  // for hours, and a QTextDocument with every block ever appended gets slow
  // to scroll and holds the memory forever. Dropping from the top keeps the
  // log view responsive and keeps the newest entries, which are the ones
  // being read. Same 1000-line window MO2's log list uses (loglist.cpp
  // MaxLines). The log FILE is never capped - only what is on screen.
  output_->setMaximumBlockCount(kMaxLines);
  output_->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(output_, &QWidget::customContextMenuRequested, this,
          &ConsolePanel::on_context_menu);
  layout->addWidget(output_);

  auto *copyShortcut = new QShortcut(QKeySequence::Copy, output_);
  connect(copyShortcut, &QShortcut::activated, this, [this]() {
    output_->copy();
  });

  QPointer<ConsolePanel> guard(this);
  // Console panel verbosity: GMM_DEBUG=1 forces Debug; otherwise the
  // diagnostics/log_level setting applies. Log file always full.
  const bool verbose     = gmm_debug_enabled();
  auto &settings         = Settings::instance();
  const bool panel_debug = verbose || settings.log_level() == "debug";
  min_level_ = panel_debug ? engine::LogLevel::Debug : engine::LogLevel::Info;
  auto &logger = engine::Logger::instance();
  logger.add_callback([guard](engine::LogLevel level, const std::string &timestamp,
                              const std::string &message) {
    auto *panel = guard.data();
    if (!panel)
      return;
    const int lvl = static_cast<int>(level);
    // The level test runs inside the queued lambda, i.e. on the panel's own
    // thread: it reads min_level_, which the Level submenu can change, so a
    // level switched in the menu takes effect on the very next line rather
    // than on the next panel construction.
    QMetaObject::invokeMethod(
        panel,
        [panel, lvl, ts = QString::fromStdString(timestamp),
         msg = QString::fromStdString(message)]() {
          if (!panel ||
              static_cast<engine::LogLevel>(lvl) < panel->min_level())
            return;
          panel->append_log(ConsolePanel::level_tag(lvl), ts, msg, lvl);
        },
        Qt::QueuedConnection);
  });
}

QString ConsolePanel::level_tag(int level) {
  switch (static_cast<engine::LogLevel>(level)) {
  case engine::LogLevel::Debug:
    return QStringLiteral("DBG");
  case engine::LogLevel::Info:
    return QStringLiteral("INF");
  case engine::LogLevel::Warn:
    return QStringLiteral("WRN");
  case engine::LogLevel::Error:
    return QStringLiteral("ERR");
  }
  return QString();
}

void ConsolePanel::append_log(const QString &tag, const QString &timestamp,
                              const QString &message, int level) {
  QTextCharFormat tag_fmt;
  switch (static_cast<engine::LogLevel>(level)) {
  case engine::LogLevel::Debug:
    tag_fmt.setForeground(QColor(60, 120, 220));
    tag_fmt.setFontWeight(QFont::Bold);
    break;
  case engine::LogLevel::Info:
    break;  // default text color
  case engine::LogLevel::Warn:
    tag_fmt.setForeground(QColor(255, 200, 0));
    tag_fmt.setFontWeight(QFont::Bold);
    break;
  case engine::LogLevel::Error:
    tag_fmt.setForeground(QColor(220, 40, 40));
    tag_fmt.setFontWeight(QFont::Bold);
    break;
  }
  QTextCharFormat ts_fmt;
  ts_fmt.setForeground(QColor(120, 120, 120));
  QTextCharFormat msg_fmt;

  auto *doc = output_->document();
  QTextCursor cursor(doc);
  cursor.movePosition(QTextCursor::End);
  cursor.insertText(QStringLiteral("[%1] ").arg(tag), tag_fmt);
  cursor.insertText(QStringLiteral("[%1] ").arg(timestamp), ts_fmt);
  cursor.insertText(message + '\n', msg_fmt);

  auto *bar = output_->verticalScrollBar();
  bar->setValue(bar->maximum());
}

void ConsolePanel::append_text(const QString &text) {
  output_->appendPlainText(text);
}

void ConsolePanel::clear() {
  output_->clear();
}

void ConsolePanel::set_min_level(engine::LogLevel level) {
  if (level == min_level_)
    return;
  min_level_ = level;
  Settings::instance().set_log_level(level == engine::LogLevel::Debug   ? "debug"
                                     : level == engine::LogLevel::Warn  ? "warn"
                                     : level == engine::LogLevel::Error ? "error"
                                                                      : "info");
  // Re-render from the logger's replay buffer rather than leaving the view as
  // it stands: the dropped lines never reached the document, so without this
  // raising the verbosity would only ever show what arrives afterwards, and
  // the error that made the user raise it would stay invisible.
  output_->clear();
  for (const auto &entry : engine::Logger::instance().replayed()) {
    if (entry.level < min_level_)
      continue;
    append_log(level_tag(static_cast<int>(entry.level)),
               QString::fromStdString(entry.timestamp),
               QString::fromStdString(entry.message),
               static_cast<int>(entry.level));
  }
}

void ConsolePanel::on_open_logs_folder() {
  const std::string dir = engine::Logger::instance().log_dir();
  if (dir.empty())
    return;
  QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(dir)));
}

void ConsolePanel::on_context_menu(const QPoint &pos) {
  QMenu menu(this);

  // A read-only text view has no selection until the user drags one, so Copy
  // is enabled from the selection rather than always on.
  const bool has_selection = output_->textCursor().hasSelection();
  menu.addAction(tr("Copy"), output_, &QPlainTextEdit::copy)->setEnabled(has_selection);
  menu.addAction(tr("Copy All"), this, [this]() {
    output_->selectAll();
    output_->copy();
  });
  menu.addSeparator();
  menu.addAction(tr("Clear"), this, &ConsolePanel::clear);

  auto *folder_action = menu.addAction(tr("Open Logs Folder"), this,
                                       &ConsolePanel::on_open_logs_folder);
  folder_action->setEnabled(!engine::Logger::instance().log_dir().empty());
  menu.addSeparator();

  // Level submenu (MO2's LogList level filter). Each entry is radio-checked
  // against the level the view is currently showing.
  auto *level_menu = menu.addMenu(tr("Level"));
  struct LevelEntry {
    engine::LogLevel level;
    const char *label;
    const char *stored;
  };
  static constexpr LevelEntry kLevels[] = {
      {engine::LogLevel::Debug, QT_TR_NOOP("Debug"), "debug"},
      {engine::LogLevel::Info, QT_TR_NOOP("Info"), "info"},
      {engine::LogLevel::Warn, QT_TR_NOOP("Warnings"), "warn"},
      {engine::LogLevel::Error, QT_TR_NOOP("Errors"), "error"},
  };
  for (const auto &entry : kLevels) {
    auto *action = level_menu->addAction(tr(entry.label));
    action->setCheckable(true);
    action->setChecked(min_level_ == entry.level);
    connect(action, &QAction::triggered, this, [this, entry]() {
      set_min_level(entry.level);
    });
  }

  menu.exec(output_->viewport()->mapToGlobal(pos));
}

}  // namespace ui