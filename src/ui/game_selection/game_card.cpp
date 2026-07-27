#include "ui/game_selection/game_card.h"

#include <QMouseEvent>
#include <QPainter>

namespace ui {

GameCard::GameCard(const GameEntry& entry, QWidget* parent)
    : QFrame(parent), entry_(entry) {
    setFixedSize(140, 160);
    setCursor(Qt::PointingHandCursor);
    setFrameShape(QFrame::StyledPanel);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 16, 12, 8);
    layout->setAlignment(Qt::AlignCenter);

    icon_label_ = new QLabel(this);
    icon_label_->setAlignment(Qt::AlignCenter);
    icon_label_->setFixedSize(64, 64);
    layout->addWidget(icon_label_, 0, Qt::AlignCenter);

    layout->addSpacing(8);

    name_label_ = new QLabel(QString::fromStdString(entry.display_name), this);
    name_label_->setAlignment(Qt::AlignCenter);
    name_label_->setWordWrap(true);
    name_label_->setMaximumWidth(120);
    QFont f = name_label_->font();
    f.setPointSize(10);
    name_label_->setFont(f);
    layout->addWidget(name_label_);

    // Style
    if (entry.installed) {
        setStyleSheet(
            "GameCard {"
            "  background: palette(base);"
            "  border: 2px solid palette(highlight);"
            "  border-radius: 8px;"
            "}"
            "GameCard:hover {"
            "  background: palette(highlight);"
            "}"
        );
    } else {
        setStyleSheet(
            "GameCard {"
            "  background: palette(mid);"
            "  border: 1px solid palette(midlight);"
            "  border-radius: 8px;"
            "}"
            "GameCard:hover {"
            "  background: palette(highlight);"
            "}"
        );
        name_label_->setEnabled(false);
    }
}

void GameCard::set_icon(const QIcon& icon) {
    icon_label_->setPixmap(icon.pixmap(64, 64));
}

void GameCard::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        emit clicked(entry_);
    }
    QFrame::mousePressEvent(event);
}

}  // namespace ui
