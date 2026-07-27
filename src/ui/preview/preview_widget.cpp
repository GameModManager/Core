#include "ui/preview/preview_widget.h"

#include <QContextMenuEvent>
#include <QFile>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>

namespace ui::preview {

PreviewWidget::PreviewWidget(QWidget* parent)
    : QLabel(parent, Qt::ToolTip | Qt::FramelessWindowHint)
    , anm2_parser_(std::make_unique<Anm2Parser>()) {
    setAttribute(Qt::WA_TranslucentBackground);
    apply_style();
    hide();

    connect(&anm2_timer_, &QTimer::timeout, this, &PreviewWidget::on_frame_timeout);

    debounce_timer_.setSingleShot(true);
    connect(&debounce_timer_, &QTimer::timeout, this, &PreviewWidget::on_debounce_fire);
}

PreviewWidget::~PreviewWidget() = default;

void PreviewWidget::apply_style() {
    QString border = border_color_.isEmpty() ? "palette(mid)" : border_color_;
    if (bg_mode_ == "solid" && !bg_color_.isEmpty()) {
        setStyleSheet(QString(
            "border: 1px solid %1; background: %2; padding: 2px;").arg(border, bg_color_));
    } else {
        setStyleSheet(QString(
            "border: 1px solid %1; padding: 2px;").arg(border));
    }
}

QPixmap PreviewWidget::make_checker(const QString& c1, const QString& c2) {
    const int size = 8;
    QPixmap pm(size * 2, size * 2);
    pm.fill(QColor(c1));
    QPainter p(&pm);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(c2));
    p.drawRect(0, 0, size, size);
    p.drawRect(size, size, size, size);
    return pm;
}

QPixmap PreviewWidget::get_checker_pixmap() {
    // Local statics: lazy-initialized on first call (guaranteed after QApplication exists).
    static QPixmap checker_light;
    static QPixmap checker_dark;

    if (bg_mode_ == "checker_light") {
        if (checker_light.isNull()) checker_light = make_checker("#ffffff", "#cccccc");
        return checker_light;
    }
    if (bg_mode_ == "checker_dark") {
        if (checker_dark.isNull()) checker_dark = make_checker("#3a3a3a", "#2e2e2e");
        return checker_dark;
    }
    // auto: detect from palette
    auto bg = palette().color(QPalette::Window);
    int lum = (bg.red() * 299 + bg.green() * 587 + bg.blue() * 114) / 1000;
    if (lum < 128) {
        if (checker_dark.isNull()) checker_dark = make_checker("#3a3a3a", "#2e2e2e");
        return checker_dark;
    }
    if (checker_light.isNull()) checker_light = make_checker("#ffffff", "#cccccc");
    return checker_light;
}

void PreviewWidget::paintEvent(QPaintEvent* event) {
    if (bg_mode_ != "solid") {
        QPainter p(this);
        p.fillRect(rect(), QBrush(get_checker_pixmap()));
        p.end();
    }
    QLabel::paintEvent(event);
}

void PreviewWidget::contextMenuEvent(QContextMenuEvent* event) {
    QMenu menu(this);
    QAction* anim_action = menu.addAction("Animate .anm2 preview");
    anim_action->setCheckable(true);
    anim_action->setChecked(animate_anm2_);
    connect(anim_action, &QAction::toggled, this, &PreviewWidget::set_animate_anm2);
    menu.exec(event->globalPos());
}

void PreviewWidget::set_animate_anm2(bool animate) {
    animate_anm2_ = animate;
    if (!animate) anm2_timer_.stop();
}

void PreviewWidget::set_background_mode(const QString& mode) {
    bg_mode_ = mode;
    apply_style();
    update();
}

void PreviewWidget::set_background_color(const QString& color) {
    bg_color_ = color;
    apply_style();
    update();
}

void PreviewWidget::set_border_color(const QString& color) {
    border_color_ = color;
    apply_style();
}

bool PreviewWidget::show_preview(const QString& file_path, const QPoint& global_pos,
                                  bool debounce) {
    QString lower = file_path.toLower();
    if (!lower.endsWith(".png") && !lower.endsWith(".anm2")) return false;
    if (!QFile::exists(file_path)) return false;

    // Cancel any pending preview
    anm2_timer_.stop();
    debounce_timer_.stop();
    anm2_frames_.clear();
    anm2_delays_.clear();
    anm2_index_ = 0;
    hide();

    if (debounce) {
        pending_path_ = file_path;
        pending_pos_ = global_pos;
        debounce_timer_.start(50);
        return true;
    }

    // Load directly
    if (lower.endsWith(".png")) {
        return try_load_png(file_path);
    } else if (lower.endsWith(".anm2")) {
        return try_load_anm2(file_path);
    }
    return false;
}

void PreviewWidget::on_debounce_fire() {
    if (!pending_path_.isEmpty()) {
        if (pending_path_.toLower().endsWith(".png")) {
            try_load_png(pending_path_);
        } else if (pending_path_.toLower().endsWith(".anm2")) {
            try_load_anm2(pending_path_);
        }
    }
}

bool PreviewWidget::try_load_png(const QString& path) {
    QPixmap pm(path);
    if (pm.isNull()) return false;

    // Scale to max 200px
    QPixmap scaled = pm.scaled(200, 200, Qt::KeepAspectRatio, Qt::FastTransformation);
    setPixmap(scaled);
    adjustSize();
    move(pending_pos_.x() + 15, pending_pos_.y() + 15);
    show();
    return true;
}

bool PreviewWidget::try_load_anm2(const QString& path) {
    auto data = anm2_parser_->parse(path.toStdString());
    if (!data) return false;

    if (!animate_anm2_ || data->frames.size() <= 1) {
        // Show first frame as static image
        if (data->frames.empty()) return false;
        const auto& first_frame = data->frames.front();
        QImage canvas(data->canvas_width, data->canvas_height,
                      QImage::Format_ARGB32_Premultiplied);
        canvas.fill(Qt::transparent);
        QPainter painter(&canvas);
        for (const auto& item : first_frame.items) {
            painter.drawImage(item.position.toPoint(), item.sprite);
        }
        painter.end();

        QPixmap pm = QPixmap::fromImage(canvas).scaled(
            200, 200, Qt::KeepAspectRatio, Qt::FastTransformation);
        setPixmap(pm);
        adjustSize();
        move(pending_pos_.x() + 15, pending_pos_.y() + 15);
        show();
        return true;
    }

    // Build animation frames
    anm2_frames_.clear();
    anm2_delays_.clear();

    for (const auto& frame : data->frames) {
        QImage canvas(data->canvas_width, data->canvas_height,
                      QImage::Format_ARGB32_Premultiplied);
        canvas.fill(Qt::transparent);
        QPainter painter(&canvas);
        for (const auto& item : frame.items) {
            painter.drawImage(item.position.toPoint(), item.sprite);
        }
        painter.end();

        anm2_frames_.push_back(QPixmap::fromImage(canvas));
        anm2_delays_.push_back(frame.delay_ms);
    }

    anm2_index_ = 0;
    setPixmap(anm2_frames_.front().scaled(
        200, 200, Qt::KeepAspectRatio, Qt::FastTransformation));
    adjustSize();

    if (anm2_frames_.size() > 1) {
        anm2_timer_.start(anm2_delays_.front());
    }

    move(pending_pos_.x() + 15, pending_pos_.y() + 15);
    show();
    return true;
}

void PreviewWidget::on_frame_timeout() {
    if (anm2_frames_.empty()) return;
    anm2_index_ = (anm2_index_ + 1) % anm2_frames_.size();
    setPixmap(anm2_frames_[anm2_index_].scaled(
        200, 200, Qt::KeepAspectRatio, Qt::FastTransformation));
    adjustSize();
    anm2_timer_.setInterval(anm2_delays_[anm2_index_]);
}

void PreviewWidget::stop() {
    debounce_timer_.stop();
    anm2_timer_.stop();
    anm2_frames_.clear();
    anm2_delays_.clear();
    anm2_index_ = 0;
    pending_path_.clear();
    hide();
}

}  // namespace ui::preview
