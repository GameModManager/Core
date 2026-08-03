#pragma once

#include "ui/viewer/file_viewer_widget.h"

#include <QImage>

class QGraphicsPixmapItem;
class QGraphicsScene;

namespace ui {

class ZoomableView;

// Full-featured image viewer built on ZoomableView: loads an image file, fits
// it to the viewport (aspect preserved), and re-fits on resize until the user
// zooms manually (Ctrl+wheel or the floating zoom bar). Drag pans.
//   - fit() / zoom_1to1() for explicit states (1.0 = native pixels).
//   - pixel_zoom() reports zoom relative to native pixels.
class ImageViewer : public FileViewerWidget {
    Q_OBJECT
public:
    explicit ImageViewer(QWidget* parent = nullptr);

    bool open(const QString& path) override;
    void clear() override;

    QImage image() const { return image_; }
    ZoomableView* view() const { return view_; }

    // Current zoom relative to the image's native pixels (1.0 = 100%).
    qreal pixel_zoom() const;

    void fit();
    void zoom_1to1();
    void zoom_in();
    void zoom_out();

signals:
    void image_loaded(const QImage& image);
    void zoom_changed(qreal pixel_zoom);

private:
    void apply_image();

    ZoomableView* view_ = nullptr;
    QGraphicsScene* scene_ = nullptr;
    QGraphicsPixmapItem* pixmap_item_ = nullptr;
    QImage image_;
};

}  // namespace ui
