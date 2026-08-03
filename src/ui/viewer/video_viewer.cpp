#include "ui/viewer/video_viewer.h"

#include <QAudioOutput>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMediaPlayer>
#include <QPushButton>
#include <QSlider>
#include <QTime>
#include <QUrl>
#include <QVBoxLayout>
#include <QVideoWidget>

namespace ui {

namespace {
QString format_time(qint64 ms) {
    return QTime::fromMSecsSinceStartOfDay(ms).toString("mm:ss");
}
}  // namespace

VideoViewer::VideoViewer(QWidget* parent) : FileViewerWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    video_ = new QVideoWidget(this);
    layout->addWidget(video_, 1);

    auto* controls = new QWidget(this);
    auto* bar = new QHBoxLayout(controls);
    bar->setContentsMargins(4, 2, 4, 2);

    play_btn_ = new QPushButton(QChar(0x25B6), controls);  // "▶"
    play_btn_->setObjectName("videoPlayBtn");
    play_btn_->setFixedSize(24, 24);
    play_btn_->setToolTip(tr("Play / Pause"));
    bar->addWidget(play_btn_);

    position_slider_ = new QSlider(Qt::Horizontal, controls);
    position_slider_->setObjectName("videoPosition");
    position_slider_->setRange(0, 0);
    bar->addWidget(position_slider_, 1);

    time_label_ = new QLabel("00:00 / 00:00", controls);
    bar->addWidget(time_label_);

    layout->addWidget(controls);

    player_ = new QMediaPlayer(this);
    audio_ = new QAudioOutput(this);
    player_->setAudioOutput(audio_);
    player_->setVideoOutput(video_);

    connect(play_btn_, &QPushButton::clicked, this, [this]() {
        if (player_->playbackState() == QMediaPlayer::PlayingState) {
            player_->pause();
        } else {
            player_->play();
        }
    });
    connect(player_, &QMediaPlayer::playbackStateChanged, this, [this](auto state) {
        play_btn_->setText(state == QMediaPlayer::PlayingState ? QChar(0x23F8)
                                                               : QChar(0x25B6));
    });
    connect(position_slider_, &QSlider::sliderPressed, this,
            [this]() { user_seeking_ = true; });
    connect(position_slider_, &QSlider::sliderReleased, this, [this]() {
        user_seeking_ = false;
        if (player_->isSeekable()) {
            player_->setPosition(position_slider_->value());
            on_position(position_slider_->value());
        }
    });
    connect(player_, &QMediaPlayer::positionChanged, this,
            &VideoViewer::on_position);
    connect(player_, &QMediaPlayer::durationChanged, this,
            &VideoViewer::on_duration);
}

VideoViewer::~VideoViewer() {
    player_->stop();
}

bool VideoViewer::open(const QString& path) {
    player_->stop();
    player_->setSource(QUrl());
    player_->setSource(QUrl::fromLocalFile(path));
    position_slider_->setRange(0, 0);
    duration_ = 0;
    time_label_->setText("00:00 / 00:00");
    set_current_path(path);
    player_->play();
    return true;
}

void VideoViewer::clear() {
    player_->stop();
    player_->setSource(QUrl());
    position_slider_->setRange(0, 0);
    duration_ = 0;
    time_label_->setText("00:00 / 00:00");
    set_current_path(QString());
}

void VideoViewer::on_position(qint64 position) {
    if (!user_seeking_ && duration_ > 0) {
        QSignalBlocker block(position_slider_);
        position_slider_->setValue(static_cast<int>(position));
    }
    time_label_->setText(tr("%1 / %2")
                             .arg(format_time(position), format_time(duration_)));
}

void VideoViewer::on_duration(qint64 duration) {
    duration_ = duration;
    position_slider_->setRange(0, static_cast<int>(duration));
}

}  // namespace ui
