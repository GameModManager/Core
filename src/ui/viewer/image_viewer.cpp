#include "ui/viewer/image_viewer.h"

#include "ui/preview/preview_widget.h"
#include "ui/settings/settings.h"
#include "ui/widgets/zoom_controls.h"

#include <QBrush>
#include <QGraphicsPixmapItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QImageReader>
#include <QPen>
#include <QPixmap>
#include <QVBoxLayout>

#include <algorithm>

namespace ui {

ImageViewer::ImageViewer(QWidget* parent) : FileViewerWidget(parent) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  view_ = new ZoomableView(this);
  view_->set_auto_fit_on_resize(true);
  layout->addWidget(view_);

  scene_ = new QGraphicsScene(view_);
  view_->setScene(scene_);
  pixmap_item_        = scene_->addPixmap(QPixmap());
  checkerboard_style_ = Settings::instance().checkerboard_style();

  connect(view_, &ZoomableView::zoom_changed, this, [this](qreal) {
    emit zoom_changed(pixel_zoom());
  });
}

bool ImageViewer::open(const QString& path) {
  // Release the previous full-res image before decoding the next one so
  // switching images never holds two full buffers at once.
  clear();
  QImageReader reader(path);
  reader.setAutoTransform(true);
  QImage img = reader.read();
  if (img.isNull()) {
    return false;
  }

  image_ = img;
  image_has_alpha_ =
      preview::path_supports_transparency(path) && preview::image_has_transparency(img);
  apply_image();
  set_current_path(path);
  emit image_loaded(image_);
  return true;
}

void ImageViewer::clear() {
  image_           = QImage();
  image_has_alpha_ = false;
  if (checker_bg_) {
    scene_->removeItem(checker_bg_);
    delete checker_bg_;
    checker_bg_ = nullptr;
  }
  pixmap_item_->setPixmap(QPixmap());
  scene_->setSceneRect(QRectF());
  set_current_path(QString());
}

qreal ImageViewer::pixel_zoom() const {
  return view_ ? view_->zoom_factor() : 1.0;
}

void ImageViewer::fit() {
  if (view_)
    view_->fit_to_scene();
}

void ImageViewer::zoom_1to1() {
  if (view_)
    view_->set_zoom(1.0, QGraphicsView::AnchorViewCenter);
}

void ImageViewer::zoom_in() {
  if (view_)
    view_->zoom_in();
}

void ImageViewer::zoom_out() {
  if (view_)
    view_->zoom_out();
}

void ImageViewer::apply_image() {
  pixmap_item_->setPixmap(QPixmap::fromImage(image_));
  scene_->setSceneRect(pixmap_item_->boundingRect());
  update_checker_background();
  fit();
}

void ImageViewer::set_checkerboard_style(int style) {
  checkerboard_style_ = std::clamp(style, 0, 3);
  update_checker_background();
}

void ImageViewer::update_checker_background() {
  if (checker_bg_) {
    scene_->removeItem(checker_bg_);
    delete checker_bg_;
    checker_bg_ = nullptr;
  }
  if (checkerboard_style_ == 0 || !image_has_alpha_ || image_.isNull())
    return;
  const QPixmap tile = preview::checker_pixmap_for_style(
      static_cast<preview::CheckerboardStyle>(checkerboard_style_));
  checker_bg_ = scene_->addRect(scene_->sceneRect(), QPen(Qt::NoPen), QBrush(tile));
  checker_bg_->setZValue(-1);
}

}  // namespace ui
