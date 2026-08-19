#include "ui/widgets/pipeline_window.h"
#include "ui/widgets/pipeline_content_widget.h"

#include <QVBoxLayout>

namespace ui {

PipelineWindow::PipelineWindow(QWidget* parent)
    : QDialog(parent) {
    setWindowTitle("Workflow Pipeline");
    setMinimumSize(460, 340);
    resize(700, 460);

    // The content widget owns its own layout margins, so the wrapper adds
    // none - the popup looks exactly like the pre-extraction dialog.
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    content_ = new PipelineContentWidget(this);
    outer->addWidget(content_);
}

void PipelineWindow::refresh() {
    content_->refresh();
}

}  // namespace ui