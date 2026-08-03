#include "ui/viewer/scene_viewer.h"

#include <QFileInfo>
#include <QLabel>
#include <QVBoxLayout>

namespace ui {

SceneViewer::SceneViewer(QWidget* parent) : FileViewerWidget(parent) {
    auto* layout = new QVBoxLayout(this);

    layout->addStretch(1);
    name_label_ = new QLabel(this);
    name_label_->setAlignment(Qt::AlignCenter);
    name_label_->setWordWrap(true);
    name_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(name_label_);

    message_ = new QLabel(tr("3D preview is not available for this file yet."), this);
    message_->setAlignment(Qt::AlignCenter);
    message_->setEnabled(false);
    layout->addWidget(message_);

    layout->addStretch(1);
}

bool SceneViewer::open(const QString& path) {
    name_label_->setText(QFileInfo(path).fileName());
    set_current_path(path);
    return true;
}

void SceneViewer::clear() {
    name_label_->clear();
    set_current_path(QString());
}

}  // namespace ui
