#pragma once

#include <QDialog>

namespace ui {

class PipelineContentWidget;

// Popup wrapper around PipelineContentWidget.  Used when Full UI mode is OFF
// (standalone window); Full UI tab mode embeds the content widget directly
// in MainTabContainer.
class PipelineWindow : public QDialog {
    Q_OBJECT
public:
    explicit PipelineWindow(QWidget* parent = nullptr);

    // Forwards to the embedded PipelineContentWidget.
    void refresh();

private:
    PipelineContentWidget* content_ = nullptr;
};

}  // namespace ui