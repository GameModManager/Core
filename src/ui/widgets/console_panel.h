#pragma once

#include <QFrame>
#include <QTextEdit>

namespace engine { class Logger; }

namespace ui {

class ConsolePanel : public QFrame {
    Q_OBJECT
public:
    explicit ConsolePanel(QWidget* parent = nullptr);

    void append_text(const QString& text);
    void append_log(const QString& tag, const QString& timestamp, const QString& message, int level);
    void clear();

private:
    QTextEdit* console_output_ = nullptr;
};

}  // namespace ui
