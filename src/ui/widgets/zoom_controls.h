#pragma once

#include <QGraphicsView>

class QFrame;
class QLabel;
class QPushButton;
class QSlider;

namespace ui {

using ViewAnchor = QGraphicsView::ViewportAnchor;

// 2D canvas view with zoom support and a floating zoom bar pinned to the
// bottom-right corner of the viewport.  Zoom state lives on the view (a
// view transform), so it survives scene rebuilds.  Plain wheel scrolls;
// Ctrl+wheel zooms under the mouse; scroll-hand drag pans.
class ZoomableView : public QGraphicsView {
    Q_OBJECT
public:
    explicit ZoomableView(QWidget* parent = nullptr);

    qreal zoom_factor() const { return factor_; }

    // Clamped to [min_zoom_, max_zoom_]; syncs the floating bar.
    // `anchor` is ViewAnchor: ViewCenter for bar controls, UnderMouse for
    // Ctrl+wheel so the canvas zooms toward the cursor.
    void set_zoom(qreal factor, ViewAnchor anchor);
    void zoom_in();
    void zoom_out();

protected:
    void resizeEvent(QResizeEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    void position_bar();
    void sync_bar();

    static constexpr qreal kMinZoom = 0.25;
    static constexpr qreal kMaxZoom = 4.0;
    static constexpr qreal kStep = 1.25;   // x1.25 / /1.25 per click

    qreal factor_ = 1.0;
    QFrame* bar_ = nullptr;
    QPushButton* zoom_in_btn_ = nullptr;
    QPushButton* zoom_out_btn_ = nullptr;
    QSlider* slider_ = nullptr;
    QLabel* percent_ = nullptr;
};

}  // namespace ui
