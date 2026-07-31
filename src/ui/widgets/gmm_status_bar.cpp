#include "ui/widgets/gmm_status_bar.h"

#include "engine/trace/trace_recorder.h"

#include <QFrame>
#include <QToolButton>

namespace ui {

namespace {
// Well-known flow ids shown in the pipeline indicator.
const char* kFlowIds[] = {"launch", "install", "sort"};
const char* kFlowTitles[] = {"Launch", "Install", "Sort"};
}  // namespace

GmmStatusBar::GmmStatusBar(QWidget* parent)
    : QWidget(parent) {
    layout_ = new QHBoxLayout(this);
    layout_->setContentsMargins(6, 2, 6, 2);
    layout_->setSpacing(12);

    // Left: general status
    status_label_ = new QLabel(tr("Ready"), this);
    layout_->addWidget(status_label_);

    layout_->addStretch();

    // Pipeline activity indicator - click to open the pipeline window
    pipeline_button_ = new QToolButton(this);
    pipeline_button_->setObjectName("pipelineIndicator");
    pipeline_button_->setText("Pipeline: idle");
    pipeline_button_->setToolTip("Workflow pipeline - click to open");
    pipeline_button_->setAutoRaise(true);
    layout_->addWidget(pipeline_button_);
    connect(pipeline_button_, &QToolButton::clicked,
            this, &GmmStatusBar::pipeline_clicked);

    // Right side: counter + sources (populated dynamically)
    counter_label_ = new QLabel(this);
    counter_label_->setObjectName("counterLabel");
    layout_->addWidget(counter_label_);

    separator_ = new QFrame();
    separator_->setFrameShape(QFrame::VLine);
    separator_->setFrameShadow(QFrame::Sunken);
    layout_->addWidget(separator_);

    pipeline_timer_ = new QTimer(this);
    connect(pipeline_timer_, &QTimer::timeout,
            this, &GmmStatusBar::refresh_pipeline_indicator);
    pipeline_timer_->start(2000);

    refresh_pipeline_indicator();
}

void GmmStatusBar::set_status(const QString& text) {
    status_label_->setText(text);
}

void GmmStatusBar::set_counter_label(const QString& label) {
    counter_label_->setText(label + ": 0");
}

void GmmStatusBar::set_counter_value(int count) {
    auto text = counter_label_->text();
    auto colon = text.indexOf(':');
    if (colon >= 0) {
        counter_label_->setText(text.left(colon + 1) + " " + QString::number(count));
    }
}

void GmmStatusBar::set_sources(const QStringList& sources) {
    // Remove old source labels
    for (auto* label : source_labels_) {
        layout_->removeWidget(label);
        label->deleteLater();
    }
    source_labels_.clear();

    // Remove old separator
    if (separator_) {
        layout_->removeWidget(separator_);
        separator_->deleteLater();
        separator_ = nullptr;
    }

    // Add separator if we have sources
    if (!sources.isEmpty()) {
        separator_ = new QFrame();
        separator_->setFrameShape(QFrame::VLine);
        separator_->setFrameShadow(QFrame::Sunken);
        layout_->addWidget(separator_);
    }

    // Add a label for each source
    for (const auto& source : sources) {
        auto* label = new QLabel(source + ": --", this);
        label->setStyleSheet("color: gray;");
        layout_->addWidget(label);
        source_labels_.append(label);
    }
}

void GmmStatusBar::refresh_pipeline_indicator() {
    auto& trace = engine::TraceRecorder::instance();
    QString text;
    bool any_running = false;

    for (int i = 0; i < 3; ++i) {
        auto snap = trace.snapshot(kFlowIds[i]);
        if (snap && snap->running) {
            if (!any_running) {
                text = QString("Pipeline: %1 running…").arg(kFlowTitles[i]);
                any_running = true;
            }
        }
    }
    if (!any_running) {
        text = "Pipeline: idle";
    }
    pipeline_button_->setText(text);
}

}  // namespace ui
