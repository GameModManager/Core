#pragma once

#include <QAbstractItemView>
#include <QEasingCurve>
#include <QEvent>
#include <QObject>
#include <QScrollBar>
#include <QTimer>
#include <QWheelEvent>
#include <QWidget>

namespace ui {

// Animates wheel scrolling on item views. QAbstractItemView applies wheel
// deltas as a single jump even with ScrollPerPixel mode; this intercepts
// wheel events and eases the scrollbar toward the target so the wheel feels
// smooth (like mouse-drag scrolling). Rapid wheel events extend the target
// instead of restarting, so the animation keeps flowing.
class SmoothScroller : public QObject {
public:
    explicit SmoothScroller(QAbstractItemView* view)
        : QObject(view), view_(view) {
        timer_.setInterval(16);
        connect(&timer_, &QTimer::timeout, this, &SmoothScroller::tick);
        view_->viewport()->installEventFilter(this);
    }

    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == view_->viewport() && event->type() == QEvent::Wheel) {
            handle_wheel(static_cast<QWheelEvent*>(event));
            return true;  // consume; we drive the scrollbar ourselves
        }
        return QObject::eventFilter(watched, event);
    }

private:
    struct Anim {
        QScrollBar* bar = nullptr;
        qreal from = 0.0;
        qreal to = 0.0;
        qreal elapsed = 0.0;
        qreal last_set = 0.0;
        bool active = false;
    };

    static constexpr int kLinesPerNotch = 3;
    static constexpr qreal kDurationMs = 180.0;

    void handle_wheel(QWheelEvent* we) {
        const QPoint angle = we->angleDelta();
        const QPoint pixel = we->pixelDelta();

        QScrollBar* bar = nullptr;
        int delta = 0;
        // Qt scrollbars move opposite to the wheel delta (QAbstractScrollArea
        // does value - delta), so negate to keep native scroll direction.
        if (!pixel.isNull()) {
            // Touchpad: pixel delta already.
            if (qAbs(pixel.y()) >= qAbs(pixel.x())) {
                bar = view_->verticalScrollBar();
                delta = -pixel.y();
            } else {
                bar = view_->horizontalScrollBar();
                delta = -pixel.x();
            }
        } else if (angle.y() != 0) {
            bar = view_->verticalScrollBar();
            delta = -qRound(angle.y() / 120.0 * bar->singleStep() * kLinesPerNotch);
        } else if (angle.x() != 0) {
            bar = view_->horizontalScrollBar();
            delta = -qRound(angle.x() / 120.0 * bar->singleStep() * kLinesPerNotch);
        }
        if (!bar || delta == 0) return;

        Anim& anim = (bar == view_->verticalScrollBar()) ? v_ : h_;
        anim.bar = bar;
        const int min = bar->minimum();
        const int max = bar->maximum();
        if (!anim.active) {
            anim.from = bar->value();
            anim.to = qBound(min, qRound(anim.from) + delta, max);
            anim.elapsed = 0.0;
            anim.last_set = anim.from;
            anim.active = true;
            timer_.start();
        } else {
            // Mid-animation: keep the same start/easing, just extend the target.
            anim.to = qBound(min, qRound(anim.to) + delta, max);
        }
    }

    void tick() {
        bool any = false;
        for (Anim* a : {&v_, &h_}) {
            if (!a->active) continue;
            if (qAbs(a->bar->value() - a->last_set) > 1.0) {
                a->active = false;  // external change (user dragged thumb etc.)
                continue;
            }
            a->elapsed += timer_.interval();
            const qreal t = qMin<qreal>(1.0, a->elapsed / kDurationMs);
            const qreal eased = curve_.valueForProgress(t);
            const qreal value = a->from + (a->to - a->from) * eased;
            a->bar->setValue(qRound(value));
            a->last_set = value;
            if (t >= 1.0) {
                a->active = false;
            } else {
                any = true;
            }
        }
        if (!any) timer_.stop();
    }

    QAbstractItemView* view_;
    QTimer timer_;
    QEasingCurve curve_{QEasingCurve::OutCubic};
    Anim v_;
    Anim h_;
};

// Enables smooth scrolling on every item view under `root`:
//  - ScrollPerPixel scroll mode (pixel-precise wheel mapping), and
//  - a SmoothScroller that animates wheel events (wheel would otherwise jump).
// QScrollArea and text widgets already scroll per-pixel in Qt 6, so only
// item views need this.
//
// TODO: gate behind a Settings "Smooth scrolling" checkbox instead of
// calling this unconditionally.
inline void enable_smooth_scrolling(QWidget* root) {
    if (!root) return;
    const auto views = root->findChildren<QAbstractItemView*>();
    for (auto* view : views) {
        view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        view->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
        new SmoothScroller(view);
    }
}

}  // namespace ui
