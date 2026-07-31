#include "ui/widgets/pipeline_window.h"
#include "ui/widgets/zoom_controls.h"

#include "engine/trace/trace_recorder.h"

#include <QGraphicsPathItem>
#include <QGraphicsPolygonItem>
#include <QGraphicsProxyWidget>
#include <QGraphicsScene>
#include <QHBoxLayout>
#include <QPainterPath>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdio>

namespace ui {

namespace {

// Well-known flow ids, in tab order.
const char* kFlowIds[] = {"launch", "install", "sort"};
const char* kFlowLabels[] = {"Launch", "Install", "Sort"};

// Card geometry.
const int kCardWidth = 240;
const int kGap = 56;   // horizontal distance between cards (connector runs here)
const int kMargin = 18;

const char* kFailColor = "#c62828";    // semantic red (matches status_color Failed)
const char* kWarnColor = "#f9a825";    // amber for skipped / warning states

}  // namespace

PipelineWindow::PipelineWindow(QWidget* parent)
    : QDialog(parent) {
    setWindowTitle("Workflow Pipeline");
    setMinimumSize(460, 340);
    resize(700, 460);

    auto* outer = new QVBoxLayout(this);
    outer->setSpacing(8);

    tabs_ = new QTabWidget(this);
    outer->addWidget(tabs_);

    for (int i = 0; i < 3; ++i) {
        auto* tab = new QWidget(this);
        auto* layout = new QVBoxLayout(tab);
        layout->setContentsMargins(8, 8, 8, 8);
        layout->setSpacing(6);

        auto* header = new QLabel(kFlowLabels[i], tab);
        header->setObjectName("pipelineHeader");
        layout->addWidget(header);

        auto* view = new ZoomableView(tab);
        auto* scene = new QGraphicsScene(view);
        view->setScene(scene);
        layout->addWidget(view, 1);

        FlowTab flow;
        flow.header = header;
        flow.view = view;
        flow.scene = scene;
        tabs_by_flow_[kFlowIds[i]] = flow;

        tabs_->addTab(tab, kFlowLabels[i]);
    }

    refresh();

    refresh_timer_ = new QTimer(this);
    connect(refresh_timer_, &QTimer::timeout, this, &PipelineWindow::refresh);
    refresh_timer_->start(2000);
}

PipelineWindow::~PipelineWindow() {
    if (refresh_timer_) refresh_timer_->stop();
}

QString PipelineWindow::status_color(engine::TraceStatus status) {
    switch (status) {
        case engine::TraceStatus::Running:      return "#1e88e5";
        case engine::TraceStatus::Completed:    return "#2e7d32";
        case engine::TraceStatus::Failed:       return kFailColor;
        case engine::TraceStatus::Skipped:      return "#757575";
        case engine::TraceStatus::NotImplemented:
        case engine::TraceStatus::Pending:      return "#9e9e9e";
    }
    return "#9e9e9e";
}

QString PipelineWindow::status_text(engine::TraceStatus status) {
    switch (status) {
        case engine::TraceStatus::Running:      return "Running";
        case engine::TraceStatus::Completed:    return "Completed";
        case engine::TraceStatus::Failed:       return "Failed";
        case engine::TraceStatus::Skipped:      return "Skipped";
        case engine::TraceStatus::NotImplemented: return "Not implemented";
        case engine::TraceStatus::Pending:      return "Waiting";
    }
    return "Waiting";
}

QString PipelineWindow::format_duration(int64_t ms) {
    if (ms <= 0) return QString();
    if (ms < 1000) return QString::number(ms) + " ms";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f s", static_cast<double>(ms) / 1000.0);
    return QString::fromLatin1(buf);
}

void PipelineWindow::show_placeholder(FlowTab& tab, const QString& text) {
    tab.scene->clear();
    tab.cards.clear();
    tab.connectors.clear();
    tab.stage_names.clear();

    auto* label = new QLabel(text);
    label->setObjectName("pipelinePlaceholder");
    auto* proxy = tab.scene->addWidget(label);
    proxy->setPos(kMargin, kMargin);
    tab.scene->setSceneRect(0, 0, 320, 80);
}

void PipelineWindow::rebuild_cards(FlowTab& tab,
                                   const std::vector<engine::TraceStage>& stages) {
    tab.scene->clear();
    tab.cards.clear();
    tab.connectors.clear();
    tab.stage_names.clear();

    if (stages.empty()) return;

    qreal x = kMargin;
    for (const auto& stage : stages) {
        StageCard card;

        auto* w = new QWidget;
        w->setObjectName("pipelineCard");
        w->setFixedWidth(kCardWidth);
        auto* v = new QVBoxLayout(w);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(0);

        // Header bar: dot + name (left), origin (right).
        auto* hdr = new QWidget(w);
        hdr->setObjectName("pipelineCardHeader");
        card.header = hdr;
        auto* hh = new QHBoxLayout(hdr);
        hh->setContentsMargins(8, 5, 8, 5);
        hh->setSpacing(6);

        card.dot = new QLabel("●", hdr);
        card.dot->setFixedWidth(12);
        card.dot->setAlignment(Qt::AlignCenter);
        hh->addWidget(card.dot);

        card.name = new QLabel(QString::fromStdString(stage.name), hdr);
        card.name->setObjectName("pipelineStageName");
        hh->addWidget(card.name);

        hh->addStretch(1);

        card.origin = new QLabel(QString::fromStdString(stage.origin), hdr);
        card.origin->setObjectName("pipelineOrigin");
        hh->addWidget(card.origin);
        v->addWidget(hdr);

        // Status line.
        card.status = new QLabel(w);
        card.status->setObjectName("pipelineStatus");
        card.status->setContentsMargins(8, 6, 8, 0);
        v->addWidget(card.status);

        // Description (left) + elapsed time (right).
        auto* dl = new QWidget(w);
        auto* dh = new QHBoxLayout(dl);
        dh->setContentsMargins(8, 3, 8, 8);
        dh->setSpacing(6);
        card.description = new QLabel(QString::fromStdString(stage.description), dl);
        card.description->setObjectName("pipelineDescription");
        card.description->setWordWrap(true);
        card.description->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        dh->addWidget(card.description, 1);
        card.duration = new QLabel(dl);
        card.duration->setObjectName("pipelineDuration");
        card.duration->setAlignment(Qt::AlignTop | Qt::AlignRight);
        dh->addWidget(card.duration);
        v->addWidget(dl);

        // Failure box - hidden until the stage fails.
        card.fail_box = new QLabel(w);
        card.fail_box->setObjectName("pipelineFailBox");
        card.fail_box->setWordWrap(true);
        card.fail_box->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        card.fail_box->hide();
        v->addWidget(card.fail_box);

        w->adjustSize();
        card.proxy = tab.scene->addWidget(w);
        card.proxy->setPos(x, kMargin);

        tab.cards.push_back(std::move(card));
        tab.stage_names.push_back(stage.name);
        x += kCardWidth + kGap;
    }

    // Dependency connectors: card i -> card i+1.
    for (size_t i = 0; i + 1 < tab.cards.size(); ++i) {
        Connector conn;
        conn.left = static_cast<int>(i);
        conn.right = static_cast<int>(i + 1);
        conn.line = tab.scene->addPath(QPainterPath());
        conn.head = tab.scene->addPolygon(QPolygonF(),
                                          QPen(Qt::NoPen), QBrush());
        tab.connectors.push_back(std::move(conn));
    }

    relayout(tab);
}

void PipelineWindow::update_cards(FlowTab& tab,
                                  const std::vector<engine::TraceStage>& stages) {
    for (size_t i = 0; i < tab.cards.size() && i < stages.size(); ++i) {
        const auto& stage = stages[i];
        auto& card = tab.cards[i];

        auto color = status_color(stage.status);
        card.dot->setStyleSheet("color: " + color + ";");
        card.name->setStyleSheet(stage.status == engine::TraceStatus::NotImplemented
                                     ? "color: palette(mid);"
                                     : QString());
        card.status->setText(status_text(stage.status));
        card.status->setStyleSheet("color: " + color + ";");
        card.duration->setText(format_duration(stage.duration_ms));
        if (card.description && !stage.description.empty())
            card.description->setText(QString::fromStdString(stage.description));

        // Failure box: visible only when the stage failed with a message.
        bool failed = stage.status == engine::TraceStatus::Failed &&
                      !stage.reason.empty();
        card.fail_box->setText(QString::fromStdString(stage.reason));
        card.fail_box->setVisible(failed);

        // Header color: red on failure, yellow for warnings (skipped),
        // default otherwise.
        QString hdr_style;
        if (stage.status == engine::TraceStatus::Failed) {
            hdr_style = QString("background-color: rgba(198, 40, 40, 45);"
                                "border-bottom: 1px solid %1;").arg(kFailColor);
        } else if (stage.status == engine::TraceStatus::Skipped) {
            hdr_style = QString("background-color: rgba(249, 168, 37, 55);"
                                "border-bottom: 1px solid %1;").arg(kWarnColor);
        }
        card.header->setStyleSheet(hdr_style);
    }
}

void PipelineWindow::relayout(FlowTab& tab) {
    if (tab.cards.empty()) return;

    // Vertically center every card on the tallest one so connectors stay
    // horizontal regardless of per-card height (e.g. failure boxes).
    qreal max_h = 0;
    for (const auto& card : tab.cards)
        max_h = std::max(max_h, card.proxy->boundingRect().height());
    qreal cy = kMargin + max_h / 2.0;

    for (auto& card : tab.cards) {
        card.proxy->setY(cy - card.proxy->boundingRect().height() / 2.0);
    }

    // Re-draw connector lines + arrowheads between consecutive cards.
    QColor line_color = palette().mid().color();
    const qreal a_size = 6.0;
    for (auto& conn : tab.connectors) {
        if (conn.left < 0 || conn.right >= static_cast<int>(tab.cards.size()))
            continue;
        const auto& a = tab.cards[conn.left].proxy;
        const auto& b = tab.cards[conn.right].proxy;
        qreal y = cy;
        qreal x1 = a->pos().x() + a->boundingRect().width();
        qreal x2 = b->pos().x();

        QPainterPath path;
        path.moveTo(x1, y);
        path.lineTo(x2 - a_size, y);
        conn.line->setPath(path);
        conn.line->setPen(QPen(line_color, 1.5));

        QPolygonF tri;
        tri << QPointF(x2 - a_size, y - a_size)
            << QPointF(x2 - a_size, y + a_size)
            << QPointF(x2, y);
        conn.head->setPolygon(tri);
        conn.head->setPen(QPen(Qt::NoPen));
        conn.head->setBrush(QBrush(line_color));
    }

    tab.scene->setSceneRect(
        tab.scene->itemsBoundingRect().adjusted(-kMargin, -kMargin, kMargin, kMargin));
}

void PipelineWindow::refresh() {
    for (auto& [flow_id, tab] : tabs_by_flow_) {
        auto snap = engine::TraceRecorder::instance().snapshot(flow_id);
        if (!snap) {
            show_placeholder(tab, "This flow has not run yet.");
            continue;
        }

        QString state;
        if (snap->running) {
            state = "Running…";
        } else if (!snap->started) {
            state = "Not started";
        } else if (!snap->stages.empty()) {
            state = snap->success ? "Completed" : "Failed";
        }
        tab.header->setText(QString("%1: %2")
                                .arg(QString::fromStdString(snap->title))
                                .arg(state));

        if (snap->stages.empty()) {
            show_placeholder(tab, "This flow has not run yet.");
            continue;
        }

        std::vector<std::string> names;
        names.reserve(snap->stages.size());
        for (const auto& s : snap->stages) names.push_back(s.name);

        if (names != tab.stage_names) {
            rebuild_cards(tab, snap->stages);
        }
        update_cards(tab, snap->stages);
        relayout(tab);

        // Track palette changes (theme reload) so the canvas + connectors
        // stay readable on the current theme.
        QColor base = palette().base().color();
        if (tab.view->backgroundBrush().color() != base)
            tab.view->setBackgroundBrush(base);
    }
}

}  // namespace ui
