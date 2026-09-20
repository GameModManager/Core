#include "ui/preview/preview_widget.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QStringList>

#include <algorithm>

#include "engine/game/registry/game_features/game_feature.h"
#include "engine/game/registry/game_features/game_feature_registry.h"
#include <filesystem>

namespace ui::preview {

// Single checkerboard tile (8px squares). Cell layout matches the ImageDiff
// reference (CanvasView::makeCheckerPixmap): c1 top-left/bottom-right,
// c2 top-right/bottom-left.
static QPixmap checker_tile(const QColor& c1, const QColor& c2) {
  const int size = 8;
  QPixmap pm(size * 2, size * 2);
  QPainter p(&pm);
  p.fillRect(0, 0, size, size, c1);
  p.fillRect(size, 0, size, size, c2);
  p.fillRect(0, size, size, size, c2);
  p.fillRect(size, size, size, size, c1);
  p.end();
  return pm;
}

QPixmap checker_pixmap_for_style(CheckerboardStyle style) {
  // Local statics: lazy-initialized on first call (guaranteed after
  // QApplication exists).
  static QPixmap checker_light;
  static QPixmap checker_medium;
  static QPixmap checker_dark;

  switch (style) {
  case CheckerboardStyle::Light:
    if (checker_light.isNull())
      checker_light = checker_tile(QColor(204, 204, 204), QColor(153, 153, 153));
    return checker_light;
  case CheckerboardStyle::Medium:
    if (checker_medium.isNull())
      checker_medium = checker_tile(QColor(153, 153, 153), QColor(102, 102, 102));
    return checker_medium;
  case CheckerboardStyle::Dark:
    if (checker_dark.isNull())
      checker_dark = checker_tile(QColor(102, 102, 102), QColor(51, 51, 51));
    return checker_dark;
  case CheckerboardStyle::Off:
    break;
  }
  return QPixmap();
}

QIcon checkerboard_icon(int style, int extent) {
  const int half = std::max(extent / 2, 1);
  QPixmap pm(half * 2, half * 2);
  QPainter p(&pm);
  p.setPen(Qt::NoPen);
  if (style == static_cast<int>(CheckerboardStyle::Off)) {
    p.setBrush(QApplication::palette().color(QPalette::Window));
    p.drawRect(0, 0, half * 2, half * 2);
  } else {
    const QPixmap tile = checker_pixmap_for_style(
        static_cast<CheckerboardStyle>(std::clamp(style, 1, 3)));
    p.drawPixmap(0, 0, half * 2, half * 2, tile.scaled(half * 2, half * 2));
  }
  p.end();
  return QIcon(pm);
}

bool path_supports_transparency(const QString& path) {
  static const QStringList kAlphaSuffixes = {
      QStringLiteral("png"),  QStringLiteral("webp"), QStringLiteral("gif"),
      QStringLiteral("apng"), QStringLiteral("tif"),  QStringLiteral("tiff"),
      QStringLiteral("tga"),  QStringLiteral("dds"),  QStringLiteral("avif"),
      QStringLiteral("jxl"),  QStringLiteral("svg"),  QStringLiteral("ico"),
      QStringLiteral("icns"), QStringLiteral("bmp"),  QStringLiteral("anm2"),
  };
  return kAlphaSuffixes.contains(QFileInfo(path).suffix().toLower());
}

bool image_has_transparency(const QImage& image) {
  if (image.isNull() || !image.hasAlphaChannel())
    return false;
  const QImage rgba = image.convertToFormat(QImage::Format_ARGB32);
  for (int y = 0; y < rgba.height(); ++y) {
    const auto* line = reinterpret_cast<const QRgb*>(rgba.constScanLine(y));
    for (int x = 0; x < rgba.width(); ++x) {
      if (qAlpha(line[x]) != 255)
        return true;
    }
  }
  return false;
}

QPixmap checker_pixmap(const QString& mode) {
  if (mode == "checker_light")
    return checker_pixmap_for_style(CheckerboardStyle::Light);
  if (mode == "checker_medium")
    return checker_pixmap_for_style(CheckerboardStyle::Medium);
  if (mode == "checker_dark")
    return checker_pixmap_for_style(CheckerboardStyle::Dark);
  // auto: detect from palette
  auto bg = QApplication::palette().color(QPalette::Window);
  int lum = (bg.red() * 299 + bg.green() * 587 + bg.blue() * 114) / 1000;
  if (lum < 128)
    return checker_pixmap_for_style(CheckerboardStyle::Dark);
  return checker_pixmap_for_style(CheckerboardStyle::Light);
}

PreviewWidget::PreviewWidget(QWidget* parent)
    : QLabel(parent, Qt::ToolTip | Qt::FramelessWindowHint) {
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
    setStyleSheet(QString("border: 1px solid %1; background: %2; padding: 2px;")
                      .arg(border, bg_color_));
  } else {
    setStyleSheet(QString("border: 1px solid %1; padding: 2px;").arg(border));
  }
}

QPixmap PreviewWidget::make_checker(const QString& c1, const QString& c2) {
  return checker_tile(QColor(c1), QColor(c2));
}

QPixmap PreviewWidget::get_checker_pixmap() {
  return checker_pixmap(bg_mode_);
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
  QAction* anim_action = menu.addAction(tr("Animate .anm2 preview"));
  anim_action->setCheckable(true);
  anim_action->setChecked(animate_anm2_);
  connect(anim_action, &QAction::toggled, this, &PreviewWidget::set_animate_anm2);
  menu.exec(event->globalPos());
}

void PreviewWidget::set_animate_anm2(bool animate) {
  animate_anm2_ = animate;
  if (!animate)
    anm2_timer_.stop();
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
  if (!lower.endsWith(".png") && !lower.endsWith(".anm2"))
    return false;
  if (!QFile::exists(file_path))
    return false;

  // Cancel any pending preview
  anm2_timer_.stop();
  debounce_timer_.stop();
  anm2_frames_.clear();
  anm2_delays_.clear();
  anm2_index_ = 0;
  hide();

  if (debounce) {
    pending_path_ = file_path;
    pending_pos_  = global_pos;
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
  if (pm.isNull())
    return false;

  // Scale to max 200px
  QPixmap scaled = pm.scaled(200, 200, Qt::KeepAspectRatio, Qt::FastTransformation);
  setPixmap(scaled);
  adjustSize();
  move(pending_pos_.x() + 15, pending_pos_.y() + 15);
  show();
  return true;
}

bool PreviewWidget::try_load_anm2(const QString& path) {
  /* Resolve the animation parser from the game feature registry. When no
   * game-specific parser is registered, the registry's wildcard fallback
   * returns the global (non-game-specific) parser registered by a file-format
   * plugin such as ANM2. game_id_ may be empty (e.g. before set_game_id
   * is called), in which case resolve_feature uses the global parser directly.
   */
  auto feature = ::engine::Game::Features::Registry::instance()
                     .resolve_feature<::engine::AnimationParserFeature>(game_id_);
  if (!feature)
    return false;

  std::string base_dir =
      std::filesystem::path(path.toStdString()).parent_path().string();
  auto data = feature->parse(path.toStdString(), base_dir);
  if (!data)
    return false;

  if (!animate_anm2_ || data->frames.size() <= 1) {
    // Show first frame as static image
    if (data->frames.empty())
      return false;
    const auto& first_frame = data->frames.front();
    QImage canvas(data->canvas_width, data->canvas_height,
                  QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);
    QPainter painter(&canvas);
    for (const auto& layer : first_frame.layers) {
      QImage sprite(layer.rgba_pixels.data(), layer.width, layer.height,
                    QImage::Format_RGBA8888);
      painter.drawImage(QPoint(static_cast<int>(layer.x), static_cast<int>(layer.y)),
                        sprite);
    }
    painter.end();

    QPixmap pm = QPixmap::fromImage(canvas).scaled(200, 200, Qt::KeepAspectRatio,
                                                   Qt::FastTransformation);
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
    for (const auto& layer : frame.layers) {
      QImage sprite(layer.rgba_pixels.data(), layer.width, layer.height,
                    QImage::Format_RGBA8888);
      painter.drawImage(QPoint(static_cast<int>(layer.x), static_cast<int>(layer.y)),
                        sprite);
    }
    painter.end();

    anm2_frames_.push_back(QPixmap::fromImage(canvas));
    anm2_delays_.push_back(frame.delay_ms);
  }

  anm2_index_ = 0;
  setPixmap(anm2_frames_.front().scaled(200, 200, Qt::KeepAspectRatio,
                                        Qt::FastTransformation));
  adjustSize();

  if (anm2_frames_.size() > 1) {
    anm2_timer_.start(anm2_delays_.front());
  }

  move(pending_pos_.x() + 15, pending_pos_.y() + 15);
  show();
  return true;
}

void PreviewWidget::on_frame_timeout() {
  if (anm2_frames_.empty())
    return;
  anm2_index_ = (anm2_index_ + 1) % anm2_frames_.size();
  setPixmap(anm2_frames_[anm2_index_].scaled(200, 200, Qt::KeepAspectRatio,
                                             Qt::FastTransformation));
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
