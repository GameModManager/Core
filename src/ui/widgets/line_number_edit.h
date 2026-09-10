#pragma once

#include <QPlainTextEdit>
#include <QWidget>

namespace ui {

// A QPlainTextEdit with a line-number gutter painted alongside the left edge.
// Drop-in replacement: subclass adds the gutter transparently; all
// QPlainTextEdit API works unchanged.
class LineNumberPlainTextEdit : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit LineNumberPlainTextEdit(QWidget* parent = nullptr);

    int lineNumberAreaWidth() const;
    void lineNumberAreaPaintEvent(QPaintEvent* event);

protected:
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void updateLineNumberAreaWidth(int newBlockCount);
    void updateLineNumberArea(const QRect& rect, int dy);

private:
    QWidget* line_number_area_;
};

}  // namespace ui
