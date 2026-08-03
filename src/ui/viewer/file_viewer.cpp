#include "ui/viewer/file_viewer.h"

#include "ui/viewer/file_viewer_widget.h"
#include "ui/viewer/image_viewer.h"
#include "ui/viewer/scene_viewer.h"
#include "ui/viewer/video_viewer.h"

#include <QFileInfo>

namespace ui {

namespace {

const QStringList& image_suffixes() {
    static const QStringList suffixes = {
        "png", "jpg", "jpeg", "bmp", "gif", "webp", "tga", "dds",
        "svg", "ico", "icns", "jxl", "avif",
    };
    return suffixes;
}

const QStringList& video_suffixes() {
    static const QStringList suffixes = {
        "mp4", "mkv", "webm", "mov", "avi", "m4v", "mpg", "mpeg", "wmv", "flv",
    };
    return suffixes;
}

// Mesh / 3D scene formats. The renderer does not exist yet; the factory still
// routes them to SceneViewer so a future mesh viewer slots in without touching
// callers.
const QStringList& scene_suffixes() {
    static const QStringList suffixes = {
        "nif", "obj", "gltf", "glb", "dae", "fbx", "blend",
    };
    return suffixes;
}

bool in_list(const QString& suffix, const QStringList& list) {
    return list.contains(suffix, Qt::CaseInsensitive);
}

}  // namespace

FileViewer::FileViewer(QWidget* parent) : QStackedWidget(parent) {
    image_viewer_ = new ImageViewer(this);
    video_viewer_ = new VideoViewer(this);
    scene_viewer_ = new SceneViewer(this);

    addWidget(image_viewer_);
    addWidget(video_viewer_);
    addWidget(scene_viewer_);

    unsupported_label_ = new QLabel(this);
    unsupported_label_->setAlignment(Qt::AlignCenter);
    unsupported_label_->setWordWrap(true);
    unsupported_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    unsupported_label_->setEnabled(false);
    unsupported_page_ = addWidget(unsupported_label_);
}

bool FileViewer::open(const QString& path) {
    const Kind kind = kind_for(path);
    if (kind == Kind::Unknown) {
        show_unsupported(path);
        return false;
    }

    FileViewerWidget* target = nullptr;
    switch (kind) {
        case Kind::Image: target = image_viewer_; break;
        case Kind::Video: target = video_viewer_; break;
        case Kind::Scene: target = scene_viewer_; break;
        default: break;
    }

    if (!target || !target->open(path)) {
        show_unsupported(path);
        return false;
    }

    current_kind_ = kind;
    current_path_ = path;
    setCurrentWidget(target);
    return true;
}

void FileViewer::clear() {
    for (auto* widget : {static_cast<QWidget*>(image_viewer_),
                         static_cast<QWidget*>(video_viewer_),
                         static_cast<QWidget*>(scene_viewer_)}) {
        static_cast<FileViewerWidget*>(widget)->clear();
    }
    current_kind_ = Kind::Unknown;
    current_path_.clear();
}

QString FileViewer::current_path() const {
    return current_path_;
}

FileViewerWidget* FileViewer::current_viewer() const {
    if (current_kind_ == Kind::Image) return image_viewer_;
    if (current_kind_ == Kind::Video) return video_viewer_;
    if (current_kind_ == Kind::Scene) return scene_viewer_;
    return nullptr;
}

FileViewer::Kind FileViewer::kind_for(const QString& path) {
    const QString suffix =
        QFileInfo(path).suffix().toLower();
    if (in_list(suffix, image_suffixes())) return Kind::Image;
    if (in_list(suffix, video_suffixes())) return Kind::Video;
    if (in_list(suffix, scene_suffixes())) return Kind::Scene;
    return Kind::Unknown;
}

bool FileViewer::supports(const QString& path) {
    return kind_for(path) != Kind::Unknown;
}

QStringList FileViewer::supported_suffixes() {
    return image_suffixes() + video_suffixes() + scene_suffixes();
}

void FileViewer::show_unsupported(const QString& path) {
    unsupported_label_->setText(
        tr("No viewer for this file type.\n\n%1").arg(path));
    setCurrentIndex(unsupported_page_);
}

}  // namespace ui
