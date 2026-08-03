#pragma once

#include <QLabel>
#include <QStackedWidget>
#include <QStringList>

namespace ui {

class FileViewerWidget;
class ImageViewer;
class VideoViewer;
class SceneViewer;

// Reusable file-viewer container: a stack of per-type viewers (image, video,
// 3D scene, ...) plus an "unsupported" fallback page. open() routes the file
// to the matching viewer by extension; a new file type only needs a new
// FileViewerWidget subclass registered here. Used by the Mod Info Images tab
// and any other place that previews a single file.
class FileViewer : public QStackedWidget {
    Q_OBJECT
public:
    enum class Kind {
        Unknown,
        Image,
        Video,
        Scene,
    };

    explicit FileViewer(QWidget* parent = nullptr);

    // Returns false when no viewer can handle the file (the unsupported page
    // is shown and the current viewer is left untouched).
    bool open(const QString& path);
    void clear();

    QString current_path() const;
    Kind current_kind() const { return current_kind_; }
    FileViewerWidget* current_viewer() const;

    ImageViewer* image_viewer() const { return image_viewer_; }
    VideoViewer* video_viewer() const { return video_viewer_; }
    SceneViewer* scene_viewer() const { return scene_viewer_; }

    static Kind kind_for(const QString& path);
    static bool supports(const QString& path);
    static QStringList supported_suffixes();

private:
    void show_unsupported(const QString& path);

    ImageViewer* image_viewer_ = nullptr;
    VideoViewer* video_viewer_ = nullptr;
    SceneViewer* scene_viewer_ = nullptr;
    QLabel* unsupported_label_ = nullptr;
    int unsupported_page_ = -1;
    Kind current_kind_ = Kind::Unknown;
    QString current_path_;
};

}  // namespace ui
