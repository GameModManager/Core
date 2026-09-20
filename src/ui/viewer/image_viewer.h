#pragma once

#include "ui/viewer/file_viewer_widget.h"

#include <QImage>

class QGraphicsPixmapItem;
class QGraphicsRectItem;
class QGraphicsScene;

namespace ui {

class ZoomableView;

// Full-featured image viewer built on ZoomableView: loads an image file, fits
// it to the viewport (aspect preserved), and re-fits on resize until the user
// zooms manually (Ctrl+wheel or the floating zoom bar). Drag pans.
//   - fit() / zoom_1to1() for explicit states (1.0 = native pixels).
//   - pixel_zoom() reports zoom relative to native pixels.
// A transparency checkerboard (0=off, 1=light, 2=medium, 3=dark, see
// ui::preview::CheckerboardStyle) renders behind the image, but only for
// files that support transparency and actually contain transparent pixels.
class ImageViewer : public FileViewerWidget {
  Q_OBJECT
public:
  explicit ImageViewer(QWidget* parent = nullptr);

  bool open(const QString& path) override;
  void clear() override;

  QImage image() const { return image_; }
  ZoomableView* view() const { return view_; }

  void set_checkerboard_style(int style);
  int checkerboard_style() const { return checkerboard_style_; }

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
  void update_checker_background();

  ZoomableView* view_               = nullptr;
  QGraphicsScene* scene_            = nullptr;
  QGraphicsPixmapItem* pixmap_item_ = nullptr;
  QGraphicsRectItem* checker_bg_    = nullptr;
  QImage image_;
  int checkerboard_style_ = 2;
  bool image_has_alpha_   = false;
};

}  // namespace ui
