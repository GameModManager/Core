#pragma once

#include <QFrame>

class QPlainTextEdit;

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
    QPlainTextEdit* output_ = nullptr;
};

}  // namespace ui