#include "ui/game_selection/game_card.h"

#include <QHBoxLayout>
#include <QMouseEvent>

namespace ui {

GameCard::GameCard(const GameEntry& entry, QWidget* parent)
    : QFrame(parent), entry_(entry) {
    setFixedHeight(44);
    setCursor(Qt::PointingHandCursor);
    setFrameShape(QFrame::NoFrame);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 4, 10, 4);
    layout->setSpacing(12);

    icon_label_ = new QLabel(this);
    icon_label_->setFixedSize(28, 28);
    icon_label_->setAlignment(Qt::AlignCenter);
    layout->addWidget(icon_label_);

    name_label_ = new QLabel(QString::fromStdString(entry.display_name), this);
    layout->addWidget(name_label_);
    layout->addStretch(1);

    // Palette-driven styling: hover highlight for the whole row; games that
    // are registered but not detected-installed keep a muted name.
    setStyleSheet(
        "GameCard { border-radius: 6px; }"
        "GameCard:hover { background: palette(highlight); }"
        "GameCard[isGeneric=true] QLabel { font-weight: bold; }");
    if (!entry.installed) {
        name_label_->setEnabled(false);
    }
}

void GameCard::set_icon(const QIcon& icon) {
    icon_label_->setPixmap(icon.pixmap(28, 28));
}

void GameCard::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        emit clicked(entry_);
    }
    QFrame::mousePressEvent(event);
}

}  // namespace ui
