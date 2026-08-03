#include "ui/viewer/image_viewer.h"

#include "ui/widgets/zoom_controls.h"

#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QImageReader>
#include <QPixmap>
#include <QVBoxLayout>

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
    pixmap_item_ = scene_->addPixmap(QPixmap());

    connect(view_, &ZoomableView::zoom_changed, this, [this](qreal) {
        emit zoom_changed(pixel_zoom());
    });
}

bool ImageViewer::open(const QString& path) {
    QImageReader reader(path);
    reader.setAutoTransform(true);
    QImage img = reader.read();
    if (img.isNull()) {
        clear();
        return false;
    }

    image_ = img;
    apply_image();
    set_current_path(path);
    emit image_loaded(image_);
    return true;
}

void ImageViewer::clear() {
    image_ = QImage();
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
    fit();
}

}  // namespace ui
