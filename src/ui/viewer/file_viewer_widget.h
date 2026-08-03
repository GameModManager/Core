#pragma once

#include <QString>
#include <QWidget>

namespace ui {

// Base class for a single-file viewer (image, video, 3D scene, archive, ...).
// A viewer is a plain reusable widget with no knowledge of where it lives in
// the app; FileViewer is the container that picks one by file type. open()
// must return false when the file cannot be shown (the container then falls
// back to its "no viewer" page).
class FileViewerWidget : public QWidget {
    Q_OBJECT
public:
    explicit FileViewerWidget(QWidget* parent = nullptr);
    ~FileViewerWidget() override;

    virtual bool open(const QString& path) = 0;
    virtual void clear() = 0;

    QString current_path() const { return path_; }
    bool has_content() const { return !path_.isEmpty(); }

protected:
    void set_current_path(const QString& path) { path_ = path; }

private:
    QString path_;
};

}  // namespace ui
