#include "ui/widgets/console_panel.h"
#include "engine/log/logger.h"

#include <QScrollBar>
#include <QVBoxLayout>

namespace ui {

ConsolePanel::ConsolePanel(QWidget* parent)
    : QFrame(parent) {
    setFrameShape(QFrame::StyledPanel);
    setFrameShadow(QFrame::Sunken);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    console_output_ = new QTextEdit(this);
    console_output_->setReadOnly(true);
    console_output_->setFont(QFont("Monospace", 9));
    console_output_->setAcceptRichText(true);
    layout->addWidget(console_output_);

    // Connect to logger
    engine::Logger::instance().add_callback(
        [this](engine::LogLevel level, const std::string& timestamp, const std::string& message) {
            int lvl = static_cast<int>(level);
            QMetaObject::invokeMethod(this, [this, lvl, ts = QString::fromStdString(timestamp),
                                              msg = QString::fromStdString(message)]() {
                // Build tag from level
                QString tag;
                switch (static_cast<engine::LogLevel>(lvl)) {
                    case engine::LogLevel::Debug: tag = "DBG"; break;
                    case engine::LogLevel::Info:  tag = "INF"; break;
                    case engine::LogLevel::Warn:  tag = "WRN"; break;
                    case engine::LogLevel::Error: tag = "ERR"; break;
                }
                append_log(tag, ts, msg, lvl);
            }, Qt::QueuedConnection);
        });
}

void ConsolePanel::append_log(const QString& tag, const QString& timestamp,
                               const QString& message, int level) {
    // Colors: DBG=blue, INF=normal, WRN=yellow, ERR=red
    QString tag_color;
    bool bold = false;
    switch (level) {
        case 0:  tag_color = "#4488ff"; bold = true; break;  // DBG - blue
        case 1:  tag_color = "#cccccc"; break;                // INF - normal
        case 2:  tag_color = "#ffcc00"; bold = true; break;  // WRN - yellow
        case 3:  tag_color = "#ff4444"; bold = true; break;  // ERR - red
        default: tag_color = "#cccccc"; break;
    }

    QString ts_color = "#888888";  // slightly darker
    QString weight = bold ? "font-weight:bold;" : "";

    QString html = QString(
        "<span style=\"color:%1;%3font-weight:bold;\">[%2]</span> "
        "<span style=\"color:%4;\">[%5]</span> "
        "<span style=\"%3\">%6</span>"
    ).arg(tag_color, tag, weight, ts_color, timestamp, message.toHtmlEscaped());

    console_output_->append(html);
    auto* sb = console_output_->verticalScrollBar();
    sb->setValue(sb->maximum());
}

void ConsolePanel::append_text(const QString& text) {
    console_output_->append(text);
    auto* sb = console_output_->verticalScrollBar();
    sb->setValue(sb->maximum());
}

void ConsolePanel::clear() {
    console_output_->clear();
}

}  // namespace ui
