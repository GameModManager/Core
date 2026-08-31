#include "ui/widgets/status_bar.h"

#include "engine/source/nexus_auth.h"
#include "engine/core/trace/trace_recorder.h"

#include <QFrame>
#include <QToolButton>

namespace ui {

namespace {
// Well-known flow ids shown in the pipeline indicator.
const char* kFlowIds[] = {"launch", "install", "sort"};
const char* kFlowTitles[] = {"Launch", "Install", "Sort"};

// The Nexus source label shows the API budget consumed this hour/day.
// Matches the "Nexus" entry of the game's download_sources knowledge key.
const char* kRateSourceName = "Nexus";
}  // namespace

StatusBar::StatusBar(QWidget* parent)
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
            this, &StatusBar::pipeline_clicked);

    // Right side: sources (populated dynamically)
    separator_ = new QFrame();
    separator_->setFrameShape(QFrame::VLine);
    separator_->setFrameShadow(QFrame::Sunken);
    layout_->addWidget(separator_);

    pipeline_timer_ = new QTimer(this);
    connect(pipeline_timer_, &QTimer::timeout,
            this, &StatusBar::refresh_pipeline_indicator);
    pipeline_timer_->start(2000);

    refresh_pipeline_indicator();
}

void StatusBar::set_status(const QString& text) {
    status_label_->setText(text);
}

void StatusBar::set_sources(const QStringList& sources) {
    // Remove old source labels
    for (auto* label : source_labels_) {
        layout_->removeWidget(label);
        label->deleteLater();
    }
    source_labels_.clear();
    source_labels_by_name_.clear();

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
        source_labels_by_name_.insert(source, label);
    }

    refresh_nexus_source();
}

void StatusBar::refresh_nexus_source() {
    auto it = source_labels_by_name_.find(kRateSourceName);
    if (it == source_labels_by_name_.end()) return;

    // Format: "Nexus: <hourly remaining>/<daily remaining>" - the Nexus API
    // budget left this hour/day (same numbers the Settings > Sources panel
    // shows, remaining out of limit).
    const auto rl = engine::Source::Nexus::Auth::instance().get_rate_limit();
    if (rl.hourly_limit <= 0 || rl.daily_limit <= 0) {
        it.value()->setText(QString("%1: --").arg(kRateSourceName));
        return;
    }
    it.value()->setText(
        QString("%1: %2/%3").arg(kRateSourceName).arg(rl.hourly_remaining).arg(rl.daily_remaining));
}

void StatusBar::refresh_pipeline_indicator() {
    refresh_nexus_source();

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
