#include "ui/widgets/console_panel.h"
#include "ui/settings/settings.h"
#include "engine/util/debug_env.h"
#include "engine/log/logger.h"

#include <QApplication>
#include <QClipboard>
#include <QPlainTextEdit>
#include <QPointer>
#include <QScrollBar>
#include <QShortcut>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QVBoxLayout>

#include <cstdlib>

namespace ui {

ConsolePanel::ConsolePanel(QWidget* parent)
    : QFrame(parent) {
    setFrameShape(QFrame::StyledPanel);
    setFrameShadow(QFrame::Sunken);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    output_ = new QPlainTextEdit(this);
    output_->setReadOnly(true);
    output_->setUndoRedoEnabled(false);
    output_->setFont(QFont("Monospace", 9));
    output_->setFocusPolicy(Qt::StrongFocus);
    output_->setLineWrapMode(QPlainTextEdit::NoWrap);
    output_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    layout->addWidget(output_);

    auto* copyShortcut = new QShortcut(QKeySequence::Copy, output_);
    connect(copyShortcut, &QShortcut::activated, this, [this]() {
        output_->copy();
    });

    QPointer<ConsolePanel> guard(this);
    // Console panel verbosity: GMM_DEBUG=1 forces Debug; otherwise the
    // diagnostics/log_level setting applies. Log file always full.
    const bool verbose = gmm_debug_enabled();
    auto& settings = Settings::instance();
    const bool panel_debug = verbose || settings.log_level() == "debug";
    auto& logger = engine::Logger::instance();
    logger.add_callback(
        [guard, panel_debug](engine::LogLevel level, const std::string& timestamp, const std::string& message) {
            if (!panel_debug && level < engine::LogLevel::Info) return;
            auto* panel = guard.data();
            if (!panel) return;
            int lvl = static_cast<int>(level);
            QMetaObject::invokeMethod(panel, [panel, lvl, ts = QString::fromStdString(timestamp),
                                               msg = QString::fromStdString(message)]() {
                if (!panel) return;
                QString tag;
                switch (static_cast<engine::LogLevel>(lvl)) {
                    case engine::LogLevel::Debug: tag = "DBG"; break;
                    case engine::LogLevel::Info:  tag = "INF"; break;
                    case engine::LogLevel::Warn:  tag = "WRN"; break;
                    case engine::LogLevel::Error: tag = "ERR"; break;
                }
                panel->append_log(tag, ts, msg, lvl);
            }, Qt::QueuedConnection);
        });
}

void ConsolePanel::append_log(const QString& tag, const QString& timestamp,
                               const QString& message, int level) {
    QTextCharFormat tag_fmt;
    switch (static_cast<engine::LogLevel>(level)) {
        case engine::LogLevel::Debug: tag_fmt.setForeground(QColor(60, 120, 220)); tag_fmt.setFontWeight(QFont::Bold); break;
        case engine::LogLevel::Info:  break;  // default text color
        case engine::LogLevel::Warn:  tag_fmt.setForeground(QColor(255, 200, 0)); tag_fmt.setFontWeight(QFont::Bold); break;
        case engine::LogLevel::Error: tag_fmt.setForeground(QColor(220, 40, 40)); tag_fmt.setFontWeight(QFont::Bold); break;
    }
    QTextCharFormat ts_fmt;
    ts_fmt.setForeground(QColor(120, 120, 120));
    QTextCharFormat msg_fmt;

    auto* doc = output_->document();
    QTextCursor cursor(doc);
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(QStringLiteral("[%1] ").arg(tag), tag_fmt);
    cursor.insertText(QStringLiteral("[%1] ").arg(timestamp), ts_fmt);
    cursor.insertText(message + '\n', msg_fmt);

    auto* bar = output_->verticalScrollBar();
    bar->setValue(bar->maximum());
}

void ConsolePanel::append_text(const QString& text) {
    output_->appendPlainText(text);
}

void ConsolePanel::clear() {
    output_->clear();
}

}  // namespace ui