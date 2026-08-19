#pragma once

#include "engine/core/trace/trace_recorder.h"

#include <QLabel>
#include <QTabWidget>
#include <QTimer>
#include <QWidget>

#include <map>
#include <string>
#include <vector>

class QGraphicsPathItem;
class QGraphicsPolygonItem;
class QGraphicsProxyWidget;
class QGraphicsScene;

namespace ui {

class ZoomableView;

// Live view of the workflow pipelines (launch / install / sort).
// Polls the engine TraceRecorder on a timer and renders each flow's
// stages as cards laid out left -> right on a 2D canvas, connected by
// arrowed dependency lines.  Read-only - visualization only.
//
// Mode-agnostic content widget: it renders inside any container - the
// PipelineWindow dialog (popup mode) or a MainTabContainer tab page
// (Full UI tab mode).
class PipelineContentWidget : public QWidget {
    Q_OBJECT
public:
    explicit PipelineContentWidget(QWidget* parent = nullptr);
    ~PipelineContentWidget() override;

    // Re-reads the TraceRecorder and redraws every flow canvas.
    void refresh();

private:
    struct StageCard {
        QWidget* card = nullptr;       // card root (fixed width, auto height)
        QWidget* header = nullptr;     // top bar: dot + name + origin
        QLabel* dot = nullptr;
        QLabel* name = nullptr;
        QLabel* origin = nullptr;
        QLabel* status = nullptr;
        QLabel* description = nullptr;
        QLabel* duration = nullptr;
        QLabel* fail_box = nullptr;    // hidden unless the stage failed
        QGraphicsProxyWidget* proxy = nullptr;
    };
    struct Connector {
        int left = -1;                 // card index this line leaves
        int right = -1;                // card index this line enters
        QGraphicsPathItem* line = nullptr;
        QGraphicsPolygonItem* head = nullptr;
    };
    struct FlowTab {
        QLabel* header = nullptr;
        ZoomableView* view = nullptr;
        QGraphicsScene* scene = nullptr;
        std::vector<StageCard> cards;
        std::vector<Connector> connectors;
        std::vector<std::string> stage_names;  // what the cards currently render
    };

    static QString status_color(engine::TraceStatus status);
    static QString status_text(engine::TraceStatus status);
    static QString format_duration(int64_t ms);

    void rebuild_cards(FlowTab& tab, const std::vector<engine::TraceStage>& stages);
    void update_cards(FlowTab& tab, const std::vector<engine::TraceStage>& stages);
    void relayout(FlowTab& tab);
    void show_placeholder(FlowTab& tab, const QString& text);

    QTabWidget* tabs_ = nullptr;
    QTimer* refresh_timer_ = nullptr;
    std::map<std::string, FlowTab> tabs_by_flow_;
};

}  // namespace ui