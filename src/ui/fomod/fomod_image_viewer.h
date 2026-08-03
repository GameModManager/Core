#pragma once

#include <QDialog>
#include <QImage>

class QLabel;

namespace ui {

// Full-size FOMOD image viewer (simplified FOMOD Plus FomodImageViewer port):
// shows the active plugin's image maximized, scaled to fit, with a hint that
// any click or Esc closes it. The thumbnail-strip navigation is dropped - the
// wizard already previews the active image inline.
class FomodImageViewer final : public QDialog {
    Q_OBJECT
public:
    FomodImageViewer(const QImage& image, QWidget* parent = nullptr);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    void rescale();
    QImage image_;
    QLabel* label_ = nullptr;
};

}  // namespace ui
