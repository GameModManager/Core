#pragma once

#include "ui/viewer/file_viewer_widget.h"

class QAudioOutput;
class QLabel;
class QMediaPlayer;
class QPushButton;
class QSlider;
class QVideoWidget;

namespace ui {

// Video player viewer (QtMultimedia backend). Plays the file in place with a
// minimal transport bar: play/pause, position slider, and elapsed time.
// Compiled only when Qt6Multimedia is available (GMM_HAS_MULTIMEDIA); the
// header stays self-contained so the container builds without it.
class VideoViewer : public FileViewerWidget {
    Q_OBJECT
public:
    explicit VideoViewer(QWidget* parent = nullptr);
    ~VideoViewer() override;

    bool open(const QString& path) override;
    void clear() override;

    QMediaPlayer* player() const { return player_; }

private:
    void on_position(qint64 position);
    void on_duration(qint64 duration);

    QMediaPlayer* player_ = nullptr;
    QAudioOutput* audio_ = nullptr;
    QVideoWidget* video_ = nullptr;
    QPushButton* play_btn_ = nullptr;
    QSlider* position_slider_ = nullptr;
    QLabel* time_label_ = nullptr;
    bool user_seeking_ = false;
    qint64 duration_ = 0;
};

}  // namespace ui
