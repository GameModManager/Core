#include "ui/fomod/fomod_image_viewer.h"

#include <QLabel>
#include <QMouseEvent>
#include <QVBoxLayout>
#include <QResizeEvent>

namespace ui {

FomodImageViewer::FomodImageViewer(const QImage& image, QWidget* parent)
    : QDialog(parent)
    , image_(image)
{
    setWindowTitle(tr("FOMOD Image"));
    setWindowFlags(Qt::Window);
    resize(800, 600);

    auto* layout = new QVBoxLayout(this);
    auto* hint = new QLabel(tr("Click anywhere or press Esc to close."), this);
    hint->setAlignment(Qt::AlignCenter);
    layout->addWidget(hint);

    label_ = new QLabel(this);
    label_->setAlignment(Qt::AlignCenter);
    layout->addWidget(label_, 1);
    rescale();
}

void FomodImageViewer::resizeEvent(QResizeEvent* event)
{
    QDialog::resizeEvent(event);
    rescale();
}

void FomodImageViewer::mousePressEvent(QMouseEvent* event)
{
    close();
    QDialog::mousePressEvent(event);
}

void FomodImageViewer::rescale()
{
    if (image_.isNull() || !label_) {
        return;
    }
    label_->setPixmap(QPixmap::fromImage(image_).scaled(
        label_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

}  // namespace ui
