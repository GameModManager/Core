#include "ui/widgets/zoom_controls.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace ui {

ZoomableView::ZoomableView(QWidget* parent)
    : QGraphicsView(parent) {
    setRenderHint(QPainter::Antialiasing);
    setDragMode(QGraphicsView::ScrollHandDrag);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);

    // Floating zoom bar, pinned to the bottom-right of the viewport.
    bar_ = new QFrame(this);
    bar_->setObjectName("zoomControls");
    auto* lay = new QHBoxLayout(bar_);
    lay->setContentsMargins(4, 3, 4, 3);
    lay->setSpacing(3);

    zoom_out_btn_ = new QPushButton("-", bar_);
    zoom_out_btn_->setObjectName("zoomOutBtn");
    zoom_out_btn_->setFixedSize(22, 22);
    zoom_out_btn_->setToolTip("Zoom out");
    lay->addWidget(zoom_out_btn_);

    slider_ = new QSlider(Qt::Horizontal, bar_);
    slider_->setObjectName("zoomSlider");
    slider_->setRange(static_cast<int>(kMinZoom * 100.0),
                      static_cast<int>(kMaxZoom * 100.0));
    slider_->setValue(100);
    slider_->setFixedWidth(110);
    slider_->setToolTip("Zoom level");
    lay->addWidget(slider_);

    zoom_in_btn_ = new QPushButton("+", bar_);
    zoom_in_btn_->setObjectName("zoomInBtn");
    zoom_in_btn_->setFixedSize(22, 22);
    zoom_in_btn_->setToolTip("Zoom in");
    lay->addWidget(zoom_in_btn_);

    percent_ = new QLabel("100%", bar_);
    percent_->setObjectName("zoomPercent");
    lay->addWidget(percent_);

    bar_->adjustSize();
    bar_->raise();

    connect(zoom_in_btn_, &QPushButton::clicked, this, &ZoomableView::zoom_in);
    connect(zoom_out_btn_, &QPushButton::clicked, this, &ZoomableView::zoom_out);
    connect(slider_, &QSlider::valueChanged, this, [this](int value) {
        set_zoom(static_cast<qreal>(value) / 100.0,
                 QGraphicsView::AnchorViewCenter);
    });
}

void ZoomableView::set_zoom(qreal factor, ViewAnchor anchor) {
    factor = std::clamp(factor, kMinZoom, kMaxZoom);
    if (std::abs(factor - factor_) < 0.0005) return;

    qreal delta = factor / factor_;
    factor_ = factor;

    setTransformationAnchor(anchor);
    scale(delta, delta);

    sync_bar();
}

void ZoomableView::zoom_in() {
    set_zoom(factor_ * kStep, QGraphicsView::AnchorViewCenter);
}

void ZoomableView::zoom_out() {
    set_zoom(factor_ / kStep, QGraphicsView::AnchorViewCenter);
}

void ZoomableView::resizeEvent(QResizeEvent* event) {
    QGraphicsView::resizeEvent(event);
    position_bar();
}

void ZoomableView::wheelEvent(QWheelEvent* event) {
    // Ctrl+wheel zooms under the cursor; plain wheel scrolls the canvas.
    if (event->modifiers() & Qt::ControlModifier) {
        qreal steps = event->angleDelta().y() / 120.0;
        if (steps != 0.0) {
            set_zoom(factor_ * std::pow(kStep, steps),
                     QGraphicsView::AnchorUnderMouse);
        }
        event->accept();
        return;
    }
    QGraphicsView::wheelEvent(event);
}

void ZoomableView::position_bar() {
    if (!bar_) return;
    bar_->move(viewport()->width() - bar_->width() - 8,
               viewport()->height() - bar_->height() - 8);
}

void ZoomableView::sync_bar() {
    QSignalBlocker block(slider_);
    int pct = qRound(factor_ * 100.0);
    slider_->setValue(pct);
    percent_->setText(QString::number(pct) + "%");
}

}  // namespace ui
