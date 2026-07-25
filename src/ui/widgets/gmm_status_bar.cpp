#include "ui/widgets/gmm_status_bar.h"

#include <QFrame>

namespace ui {

GmmStatusBar::GmmStatusBar(QWidget* parent)
    : QWidget(parent) {
    layout_ = new QHBoxLayout(this);
    layout_->setContentsMargins(6, 2, 6, 2);
    layout_->setSpacing(12);

    // Left: general status
    status_label_ = new QLabel("Ready", this);
    layout_->addWidget(status_label_);

    layout_->addStretch();

    // Right side: platform integration info
    plugin_label_ = new QLabel("Plugins: 0", this);
    plugin_label_->setStyleSheet("color: gray;");
    layout_->addWidget(plugin_label_);

    auto* sep = new QFrame();
    sep->setFrameShape(QFrame::VLine);
    sep->setFrameShadow(QFrame::Sunken);
    layout_->addWidget(sep);

    nexus_label_ = new QLabel("Nexus: --", this);
    nexus_label_->setStyleSheet("color: gray;");
    layout_->addWidget(nexus_label_);
}

void GmmStatusBar::set_status(const QString& text) {
    status_label_->setText(text);
}

void GmmStatusBar::set_nexus_info(const QString& info) {
    nexus_label_->setText("Nexus: " + info);
}

void GmmStatusBar::set_plugin_count(int count) {
    plugin_label_->setText(QString("Plugins: %1").arg(count));
}

}  // namespace ui
