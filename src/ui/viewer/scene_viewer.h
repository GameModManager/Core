#pragma once

#include "ui/viewer/file_viewer_widget.h"

class QLabel;

namespace ui {

// 3D scene viewer placeholder. This is the extension point for the future
// mesh/space viewer (nif/obj/gltf rendering, camera orbit, wireframe, ...):
// implement rendering in open() and surface it here so FileViewer keeps
// routing mesh extensions to it. Until then it shows a friendly placeholder.
class SceneViewer : public FileViewerWidget {
    Q_OBJECT
public:
    explicit SceneViewer(QWidget* parent = nullptr);

    bool open(const QString& path) override;
    void clear() override;

private:
    QLabel* name_label_ = nullptr;
    QLabel* message_ = nullptr;
};

}  // namespace ui
