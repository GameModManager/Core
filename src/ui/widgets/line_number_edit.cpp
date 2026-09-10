#include "ui/widgets/line_number_edit.h"

#include <QFontMetrics>
#include <QPainter>
#include <QPaintEvent>
#include <QScrollBar>
#include <QTextBlock>

namespace ui {

// ---------------------------------------------------------------------------
// LineNumberArea — thin companion widget that paints the gutter
// ---------------------------------------------------------------------------
class LineNumberArea : public QWidget {
public:
    explicit LineNumberArea(LineNumberPlainTextEdit* editor)
        : QWidget(editor), editor_(editor) {}

    QSize sizeHint() const override {
        return QSize(editor_->lineNumberAreaWidth(), 0);
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        editor_->lineNumberAreaPaintEvent(event);
    }

private:
    LineNumberPlainTextEdit* editor_;
};

// ---------------------------------------------------------------------------
// LineNumberPlainTextEdit
// ---------------------------------------------------------------------------
LineNumberPlainTextEdit::LineNumberPlainTextEdit(QWidget* parent)
    : QPlainTextEdit(parent) {
    line_number_area_ = new LineNumberArea(this);

    // Slots for keeping the gutter in sync.
    connect(this, &QPlainTextEdit::blockCountChanged,
            this, &LineNumberPlainTextEdit::updateLineNumberAreaWidth);
    connect(this, &QPlainTextEdit::updateRequest,
            this, &LineNumberPlainTextEdit::updateLineNumberArea);

    updateLineNumberAreaWidth(0);
}

int LineNumberPlainTextEdit::lineNumberAreaWidth() const {
    const int digits = std::max(1,
        static_cast<int>(std::log10(std::max(1, blockCount())))) + 1;
    const int space = 10 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
    return space;
}

void LineNumberPlainTextEdit::lineNumberAreaPaintEvent(QPaintEvent* event) {
    QPainter painter(line_number_area_);
    painter.fillRect(event->rect(), Qt::transparent);

    QTextBlock block = firstVisibleBlock();
    int blockNumber = block.blockNumber();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + qRound(blockBoundingRect(block).height());

    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            const QString number = QString::number(blockNumber + 1);
            painter.setPen(palette().color(QPalette::PlaceholderText));
            painter.drawText(0, top, line_number_area_->width() - 5,
                             fontMetrics().height(), Qt::AlignRight, number);
        }
        block = block.next();
        top = bottom;
        bottom = top + qRound(blockBoundingRect(block).height());
        ++blockNumber;
    }
}

void LineNumberPlainTextEdit::resizeEvent(QResizeEvent* event) {
    QPlainTextEdit::resizeEvent(event);
    const QRect cr = contentsRect();
    line_number_area_->setGeometry(
        cr.left(), cr.top(), lineNumberAreaWidth(), cr.height());
}

void LineNumberPlainTextEdit::updateLineNumberAreaWidth(int /*newBlockCount*/) {
    setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
}

void LineNumberPlainTextEdit::updateLineNumberArea(const QRect& rect, int dy) {
    if (dy)
        line_number_area_->scroll(0, dy);
    else
        line_number_area_->update(0, rect.y(), line_number_area_->width(),
                                  rect.height());

    if (rect.contains(viewport()->rect()))
        updateLineNumberAreaWidth(0);
}

}  // namespace ui
