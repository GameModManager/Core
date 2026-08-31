#include "ui/preview/preview_window.h"
#include "engine/core/log/logger.h"
#include "ui/preview/preview_registry.h"
#include "ui/preview/preview_widget.h"

#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSlider>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <filesystem>

#include "engine/game/registry/game_features/game_feature.h"
#include "engine/game/registry/game_features/game_feature_registry.h"

namespace ui::preview {

// ---------------------------------------------------------------------------
// SpeedSlider - custom QSlider with a "1x" tick mark and min/max labels
// ---------------------------------------------------------------------------

static constexpr int kSnapValues[] = {25, 50, 75, 100, 150, 200, 300, 400};
static constexpr int kSnapThreshold = 5;

SpeedSlider::SpeedSlider(QWidget *parent) : QSlider(Qt::Horizontal, parent) {
  setRange(25, 400);
  setValue(100);
  setToolTip(tr("Playback speed: 0.25x to 4.0x (drag near 1x to snap)"));
}

int SpeedSlider::valueToPixel(int val) const {
  // Map value to a 0..1 ratio within our range
  double ratio = static_cast<double>(val - minimum()) /
                 static_cast<double>(maximum() - minimum());
  // The groove area is inset by a few pixels from the widget edges.
  // style()->subControlRect gives us the exact groove rect, but a
  // simpler approximation works well enough for the tick mark.
  int margin = 6; // typical QSlider groove margin
  int w = width() - margin * 2;
  return margin + static_cast<int>(ratio * w);
}

void SpeedSlider::paintEvent(QPaintEvent *event) {
  QSlider::paintEvent(event);

  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing, true);

  // Draw a vertical tick mark and "1x" label at the 1x position (value=100)
  int tick_x = valueToPixel(100);
  int groove_y = height() / 2 + 8; // below the groove center
  int tick_top = groove_y;
  int tick_bot = height() - 2;

  p.setPen(QPen(palette().color(QPalette::Mid), 1));
  p.drawLine(tick_x, tick_top, tick_x, tick_bot);

  // "1x" label centered below the tick
  QFont f = p.font();
  f.setPointSizeF(f.pointSizeF() * 0.85);
  p.setFont(f);
  p.setPen(palette().color(QPalette::Text));
  p.drawText(QRect(tick_x - 12, tick_bot - 12, 24, 14),
             Qt::AlignHCenter | Qt::AlignTop, QStringLiteral("1x"));
  p.end();
}

// ---------------------------------------------------------------------------
// DebugImageLabel - QLabel subclass that draws bounding-box overlays for ANM2
// sprite diagnostics.  The three boxes show:
//   Red   = actual pixmap bounds (what's being painted)
//   Green = logical ANM2 canvas size (from the parser)
// Toggle with F12 (handled in PreviewWindow::keyPressEvent).
// ---------------------------------------------------------------------------

class DebugImageLabel : public QLabel {
public:
  using QLabel::QLabel;

  void set_canvas_size(const QSize &s) { canvas_size_ = s; }

  void set_overlay_enabled(bool enabled) {
    overlay_enabled_ = enabled;
    update();
  }

  [[nodiscard]] bool overlay_enabled() const { return overlay_enabled_; }

protected:
  void paintEvent(QPaintEvent *event) override {
    QLabel::paintEvent(event);
    if (!overlay_enabled_)
      return;

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);

    // Red dashed box around the pixmap
    if (!pixmap().isNull()) {
      QRect pixmap_rect = pixmap().rect();
      p.setPen(QPen(Qt::red, 2, Qt::DashLine));
      p.drawRect(pixmap_rect);
    }

    // Green dashed box around the logical ANM2 canvas size
    if (canvas_size_.isValid()) {
      p.setPen(QPen(Qt::green, 2, Qt::DashLine));
      p.drawRect(QRect(QPoint(0, 0), canvas_size_));
    }
  }

private:
  QSize canvas_size_;
  bool overlay_enabled_ = false;
};

namespace {

bool has_extension(const QString &path, const QStringList &exts) {
  return exts.contains(QFileInfo(path).suffix().toLower());
}

const QStringList &image_extensions() {
  static const QStringList exts = {"png", "jpg", "jpeg", "webp", "bmp", "gif"};
  return exts;
}

const QStringList &animation_extensions() {
  static const QStringList exts = {"anm2"};
  return exts;
}

const QStringList &text_extensions() {
  static const QStringList exts = {"txt",  "ini", "cfg",  "log",
                                   "json", "xml", "meta", "md"};
  return exts;
}

} // namespace

bool PreviewWindow::supports(const QString &file_path) {
  return has_extension(file_path, image_extensions()) ||
         has_extension(file_path, animation_extensions()) ||
         has_extension(file_path, text_extensions()) ||
         ui::preview::Registry::instance().has_preview(file_path.toStdString());
}

PreviewWindow::PreviewWindow(QWidget *parent) : QDialog(parent) {
  setWindowTitle(tr("Preview"));
  setMinimumSize(440, 380);
  resize(680, 520);

  auto *main_layout = new QVBoxLayout(this);

  // Filename label
  name_label_ = new QLabel(this);
  name_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  main_layout->addWidget(name_label_);

  // Previous / Next variant buttons
  auto *nav_row = new QHBoxLayout;
  prev_button_ = new QPushButton(tr("Previous"), this);
  next_button_ = new QPushButton(tr("Next"), this);
  nav_row->addWidget(prev_button_);
  nav_row->addWidget(next_button_);
  nav_row->addStretch(1);
  main_layout->addLayout(nav_row);

  // Source / variant info
  source_label_ = new QLabel(this);
  source_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  source_label_->setEnabled(false);
  main_layout->addWidget(source_label_);

  // Two-column content area: image/animation preview (left) + ANM2 controls
  // (right).  The controls widget is hidden for non-ANM2 files so the stack
  // gets the full width.
  auto *content_row = new QHBoxLayout;

  stack_ = new QStackedWidget(this);

  // Image page: scrollable image on a checkerboard background + zoom bar.
  image_page_ = new QWidget(this);
  auto *image_layout = new QVBoxLayout(image_page_);
  image_layout->setContentsMargins(0, 0, 0, 0);
  scroll_ = new QScrollArea(image_page_);
  scroll_->setWidgetResizable(true);
  scroll_->setAlignment(Qt::AlignCenter);
  image_label_ = new DebugImageLabel(scroll_);
  image_label_->setAlignment(Qt::AlignCenter);
  auto pal = image_label_->palette();
  pal.setBrush(QPalette::Base, QBrush(checker_pixmap("auto")));
  pal.setBrush(QPalette::Window, QBrush(checker_pixmap("auto")));
  image_label_->setPalette(pal);
  scroll_->viewport()->setPalette(pal);
  scroll_->setWidget(image_label_);
  image_layout->addWidget(scroll_);

  auto *zoom_bar = new QHBoxLayout;
  auto *fit_button = new QPushButton(tr("Fit"), image_page_);
  auto *zoom_out_button = new QPushButton("-", image_page_);
  auto *zoom_in_button = new QPushButton("+", image_page_);
  zoom_label_ = new QLabel("100%", image_page_);
  zoom_bar->addWidget(fit_button);
  zoom_bar->addWidget(zoom_out_button);
  zoom_bar->addWidget(zoom_in_button);
  zoom_bar->addStretch(1);
  zoom_bar->addWidget(zoom_label_);
  image_layout->addLayout(zoom_bar);

  stack_->addWidget(image_page_);

  // Text page: read-only monospace view.
  text_view_ = new QTextBrowser(this);
  text_view_->setReadOnly(true);
  QFont mono = text_view_->font();
  mono.setFamily(QStringLiteral("monospace"));
  text_view_->setFont(mono);
  stack_->addWidget(text_view_);

  // Unsupported page.
  unsupported_label_ =
      new QLabel(tr("No preview available for this file type."), this);
  unsupported_label_->setAlignment(Qt::AlignCenter);
  unsupported_label_->setEnabled(false);
  stack_->addWidget(unsupported_label_);

  // Plugin-provided preview page (v2 IPluginPreview). A plugin returns a
  // QWidget* for a registered extension; we embed it here. Created empty and
  // populated on demand by load_plugin_preview().
  plugin_page_ = new QWidget(this);
  plugin_layout_ = new QVBoxLayout(plugin_page_);
  plugin_layout_->setContentsMargins(0, 0, 0, 0);
  stack_->addWidget(plugin_page_);

  content_row->addWidget(stack_, 2);

  // ANM2 controls panel (right column). Hidden for non-ANM2 files.
  build_anm2_controls();
  anm2_controls_->setVisible(false);
  content_row->addWidget(anm2_controls_, 1);

  main_layout->addLayout(content_row, 1);

  connect(fit_button, &QPushButton::clicked, this, &PreviewWindow::set_fit);
  connect(zoom_in_button, &QPushButton::clicked, this,
          [this]() { zoom_by(1.25); });
  connect(zoom_out_button, &QPushButton::clicked, this,
          [this]() { zoom_by(0.8); });
  connect(prev_button_, &QPushButton::clicked, this, [this]() {
    if (variant_ > 0)
      --variant_;
    reload();
  });
  connect(next_button_, &QPushButton::clicked, this, [this]() {
    if (variant_ + 1 < paths_.size())
      ++variant_;
    reload();
  });
  connect(&anm2_timer_, &QTimer::timeout, this,
          &PreviewWindow::on_anm2_frame_timeout);
}

void PreviewWindow::show_file(const QString &file_path,
                              const QStringList &provider_paths,
                              const QStringList &provider_names) {
  engine::Logger::instance().debug(
      "[PreviewWindow] show_file: file=" + file_path.toStdString() +
      " providers=" + std::to_string(provider_paths.size()));
  paths_.clear();
  names_.clear();
  paths_ << file_path;
  names_ << QString();

  // Append any provider variants (skipping the primary, already first, and
  // entries without a resolvable on-disk copy).
  for (int i = 0; i < provider_paths.size(); ++i) {
    const auto &p = provider_paths[i];
    if (p.isEmpty() || p == file_path)
      continue;
    paths_ << p;
    names_ << (i < provider_names.size() ? provider_names[i] : QString());
  }

  variant_ = 0;
  zoom_ = 1.0;
  fit_ = true;
  reload();
  show();
  raise();
  activateWindow();
}

void PreviewWindow::reload() {
  const int count = static_cast<int>(paths_.size());
  prev_button_->setEnabled(variant_ > 0);
  next_button_->setEnabled(variant_ + 1 < count);

  const QString &path = paths_[variant_];
  name_label_->setText(QFileInfo(path).fileName());
  if (variant_ > 0 && variant_ < names_.size() && !names_[variant_].isEmpty()) {
    source_label_->setText(tr("Variant %1/%2 - %3")
                               .arg(variant_ + 1)
                               .arg(count)
                               .arg(names_[variant_]));
    source_label_->setEnabled(true);
  } else if (count > 1) {
    source_label_->setText(tr("Variant %1/%2").arg(variant_ + 1).arg(count));
    source_label_->setEnabled(true);
  } else {
    source_label_->clear();
    source_label_->setEnabled(false);
  }

  zoom_label_->setText(QStringLiteral("100%"));
  anm2_timer_.stop();
  anm2_frames_.clear();
  anm2_delays_.clear();
  anm2_index_ = 0;
  anm2_states_.clear();
  anm2_current_state_ = 0;
  anm2_playing_ = false;
  if (anm2_play_btn_)
    anm2_play_btn_->setText(tr("Play"));
  // Hide ANM2 controls by default; load_anm2() will re-show them.
  if (anm2_controls_)
    anm2_controls_->setVisible(false);
  stack_->setVisible(true);
  engine::Logger::instance().debug("[PreviewWindow] reload: path=" +
                                   path.toStdString());
  engine::Logger::instance().debug(
      "[PreviewWindow] reload: trying load_plugin_preview...");
  // ANM2 files must use the host's load_anm2() path so the controls panel
  // (speed slider, play/pause, scrubber) drives the visible image_label_
  // animation. Plugin previews for ANM2 show their own internal widget which
  // is not connected to the host controls.
  if (!has_extension(path, animation_extensions()) &&
      load_plugin_preview(path)) {
    engine::Logger::instance().debug(
        "[PreviewWindow] reload: load_plugin_preview SUCCEEDED");
    return;
  }
  engine::Logger::instance().debug(
      "[PreviewWindow] reload: trying load_image...");
  if (load_image(path)) {
    engine::Logger::instance().debug(
        "[PreviewWindow] reload: load_image SUCCEEDED");
    return;
  }
  engine::Logger::instance().debug(
      "[PreviewWindow] reload: trying load_anm2...");
  if (load_anm2(path)) {
    engine::Logger::instance().debug(
        "[PreviewWindow] reload: load_anm2 SUCCEEDED");
    return;
  }
  engine::Logger::instance().debug(
      "[PreviewWindow] reload: trying load_text...");
  if (load_text(path)) {
    engine::Logger::instance().debug(
        "[PreviewWindow] reload: load_text SUCCEEDED");
    return;
  }
  engine::Logger::instance().debug(
      "[PreviewWindow] reload: NO preview found, showing unsupported");
  show_unsupported();
}

bool PreviewWindow::load_image(const QString &path) {
  if (!has_extension(path, image_extensions()))
    return false;
  QPixmap pm(path);
  if (pm.isNull())
    return false;
  current_pixmap_ = pm;
  anm2_timer_.stop();
  anm2_frames_.clear();
  anm2_delays_.clear();
  anm2_index_ = 0;
  anm2_states_.clear();
  anm2_current_state_ = 0;
  anm2_playing_ = false;
  if (anm2_play_btn_)
    anm2_play_btn_->setText(tr("Play"));
  if (anm2_controls_)
    anm2_controls_->setVisible(false);
  stack_->setVisible(true);
  stack_->setCurrentWidget(image_page_);
  apply_zoom();
  return true;
}

bool PreviewWindow::parse_anm2_data(const QString &path) {
  if (!has_extension(path, animation_extensions()))
    return false;

  auto feature =
      ::engine::Game::Features::Registry::instance()
          .resolve_feature<::engine::AnimationParserFeature>(game_id_);
  if (!feature)
    return false;

  std::string base_dir =
      std::filesystem::path(path.toStdString()).parent_path().string();
  auto data = feature->parse(path.toStdString(), base_dir);
  if (!data || data->frames.empty())
    return false;

  anm2_states_.clear();

  // Helper lambda to convert a list of raw frames into QPixmaps.
  auto convert_frames =
      [&](const std::vector<::engine::AnimationParserFeature::Frame> &src,
          int cw, int ch) {
        AnimationState state;
        state.fps = data->fps;
        for (const auto &frame : src) {
          QImage canvas(cw, ch, QImage::Format_ARGB32_Premultiplied);
          canvas.fill(Qt::transparent);
          QPainter painter(&canvas);
          for (const auto &layer : frame.layers) {
            QImage sprite(layer.rgba_pixels.data(), layer.width, layer.height,
                          QImage::Format_RGBA8888);
            painter.drawImage(
                QPoint(static_cast<int>(layer.x), static_cast<int>(layer.y)),
                sprite);
          }
          painter.end();
          state.frames.push_back(QPixmap::fromImage(canvas));
          state.delays.push_back(frame.delay_ms);
        }
        return state;
      };

  // If the file provides named animation states, use them.
  if (!data->states.empty()) {
    for (const auto &s : data->states) {
      auto state = convert_frames(s.frames, s.canvas_width, s.canvas_height);
      state.name = QString::fromStdString(s.name);
      anm2_states_.push_back(std::move(state));
    }
  } else {
    // Fallback: single unnamed state from top-level frames.
    auto state =
        convert_frames(data->frames, data->canvas_width, data->canvas_height);
    state.name = tr("Default");
    anm2_states_.push_back(std::move(state));
  }

  // Set the debug overlay canvas size from the first state's dimensions.
  if (image_label_ && !anm2_states_.empty()) {
    int cw = 0;
    int ch = 0;
    if (!data->states.empty()) {
      cw = data->states.front().canvas_width;
      ch = data->states.front().canvas_height;
    } else {
      cw = data->canvas_width;
      ch = data->canvas_height;
    }
    image_label_->set_canvas_size(QSize(cw, ch));
  }

  // Populate the animation list widget.
  if (anm2_anim_list_) {
    anm2_anim_list_->blockSignals(true);
    anm2_anim_list_->clear();
    for (const auto &s : anm2_states_) {
      int frame_count = static_cast<int>(s.frames.size());
      anm2_anim_list_->addItem(
          tr("%1 (%2 frames)").arg(s.name).arg(frame_count));
    }
    anm2_anim_list_->setCurrentRow(0);
    anm2_anim_list_->blockSignals(false);
  }

  // Show ANM2 controls panel.
  build_anm2_controls();
  anm2_controls_->setVisible(true);

  // Switch to the first animation state.
  anm2_current_state_ = 0;
  anm2_frames_ = anm2_states_.front().frames;
  anm2_delays_ = anm2_states_.front().delays;
  anm2_index_ = 0;
  anm2_playing_ = true;
  if (anm2_play_btn_)
    anm2_play_btn_->setText(tr("Pause"));

  // Start playback immediately.
  if (!anm2_frames_.empty()) {
    double speed =
        anm2_speed_slider_ ? anm2_speed_slider_->value() / 100.0 : 1.0;
    int interval = static_cast<int>(anm2_delays_.front() / speed);
    anm2_timer_.start(std::max(interval, 1));
  }

  // Update info label: "name, X frames, Y fps"
  if (anm2_info_label_) {
    const auto &state = anm2_states_.front();
    anm2_info_label_->setText(tr("%1 - %2 frames, %3 fps")
                                  .arg(state.name)
                                  .arg(static_cast<int>(state.frames.size()))
                                  .arg(state.fps));
  }

  if (!anm2_frames_.empty()) {
    current_pixmap_ = anm2_frames_.front();
  }

  update_anm2_ui();
  return true;
}

bool PreviewWindow::load_anm2(const QString &path) {
  if (!parse_anm2_data(path))
    return false;

  // Switch the stack to the image page for the animation preview.
  stack_->setVisible(true);
  stack_->setCurrentWidget(image_page_);
  apply_zoom();
  return true;
}

void PreviewWindow::on_anm2_frame_timeout() {
  if (anm2_frames_.empty())
    return;
  anm2_index_ = (anm2_index_ + 1) % anm2_frames_.size();
  current_pixmap_ = anm2_frames_[anm2_index_];
  apply_zoom();

  // Apply speed multiplier to the next interval.
  double speed = anm2_speed_slider_ ? anm2_speed_slider_->value() / 100.0 : 1.0;
  int interval = static_cast<int>(anm2_delays_[anm2_index_] / speed);
  anm2_timer_.start(std::max(interval, 1));

  update_anm2_ui();
}

bool PreviewWindow::load_text(const QString &path) {
  if (!has_extension(path, text_extensions()))
    return false;
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    return false;
  // Cap at 8 MiB so a stray huge log can't stall the UI.
  const qint64 limit = 8 * 1024 * 1024;
  QByteArray data = file.read(limit + 1);
  if (data.size() > limit)
    data = data.left(limit);
  text_view_->setPlainText(QString::fromUtf8(data));
  // Hide ANM2 controls; show the stack for the text page.
  if (anm2_controls_)
    anm2_controls_->setVisible(false);
  stack_->setVisible(true);
  stack_->setCurrentWidget(text_view_);
  return true;
}

bool PreviewWindow::load_plugin_preview(const QString &path) {
  engine::Logger::instance().debug(
      "[PreviewWindow] load_plugin_preview: path=" + path.toStdString());
  // Drop any previously embedded plugin widget before resolving the new file so
  // a fallback to a built-in preview doesn't leave a stale widget behind.
  if (plugin_widget_) {
    plugin_layout_->removeWidget(plugin_widget_);
    delete plugin_widget_;
    plugin_widget_ = nullptr;
  }

  // Ask the v2 IPluginPreview registry for this file's extension. A plugin that
  // claimed the extension returns a QWidget* (as opaque void*); we embed it.
  engine::Logger::instance().debug(
      "[PreviewWindow] load_plugin_preview: calling Registry::create_preview");
  void *w =
      ui::preview::Registry::instance().create_preview(path.toStdString());
  engine::Logger::instance().debug(
      "[PreviewWindow] load_plugin_preview: create_preview returned w=" +
      std::to_string(reinterpret_cast<uintptr_t>(w)));
  if (!w) {
    engine::Logger::instance().debug("[PreviewWindow] load_plugin_preview: no "
                                     "plugin preview, returning false");
    return false;
  }

  plugin_widget_ = reinterpret_cast<QWidget *>(w);
  plugin_layout_->addWidget(plugin_widget_);
  stack_->setCurrentWidget(plugin_page_);
  return true;
}

void PreviewWindow::show_unsupported() {
  engine::Logger::instance().debug(
      "[PreviewWindow] show_unsupported: displaying unsupported message");
  anm2_timer_.stop();
  anm2_frames_.clear();
  anm2_delays_.clear();
  anm2_index_ = 0;
  anm2_states_.clear();
  anm2_current_state_ = 0;
  anm2_playing_ = false;
  if (anm2_play_btn_)
    anm2_play_btn_->setText(tr("Play"));
  if (anm2_controls_)
    anm2_controls_->setVisible(false);
  stack_->setVisible(true);
  current_pixmap_ = QPixmap();
  if (plugin_widget_) {
    plugin_layout_->removeWidget(plugin_widget_);
    delete plugin_widget_;
    plugin_widget_ = nullptr;
  }
  stack_->setCurrentWidget(unsupported_label_);
}

void PreviewWindow::set_fit() {
  fit_ = true;
  zoom_ = 1.0;
  apply_zoom();
}

void PreviewWindow::zoom_by(double factor) {
  if (current_pixmap_.isNull())
    return;
  if (fit_) {
    // Leave fit mode from natural size.
    fit_ = false;
    zoom_ = 1.0;
  }
  zoom_ *= factor;
  zoom_ = std::clamp(zoom_, 0.05, 64.0);
  apply_zoom();
}

void PreviewWindow::apply_zoom() {
  if (current_pixmap_.isNull()) {
    image_label_->clear();
    return;
  }
  QSize target;
  if (fit_) {
    const auto vp = scroll_->viewport()->size();
    if (vp.isEmpty()) {
      image_label_->setPixmap(current_pixmap_);
      return;
    }
    target = vp;
  } else {
    target = current_pixmap_.size() * zoom_;
  }
  target = target.expandedTo(QSize(1, 1));
  // Constrain to the available column width only in fit mode so that
  // zoomed-in views can scroll beyond the viewport edge.
  if (fit_) {
    if (int col_w = scroll_->viewport()->width(); col_w > 0)
      target.setWidth(std::min(target.width(), col_w));
  }
  image_label_->setPixmap(current_pixmap_.scaled(target, Qt::KeepAspectRatio,
                                                 Qt::SmoothTransformation));
  if (!fit_)
    zoom_label_->setText(QString::number(qRound(zoom_ * 100.0)) + "%");
  else
    zoom_label_->setText(QStringLiteral("100%"));
}

void PreviewWindow::resizeEvent(QResizeEvent *event) {
  QDialog::resizeEvent(event);
  if (fit_ && !anm2_frames_.empty()) {
    apply_zoom();
  } else if (fit_ && stack_ && stack_->isVisible() &&
             stack_->currentWidget() == image_page_) {
    apply_zoom();
  }
}

void PreviewWindow::keyPressEvent(QKeyEvent *event) {
  if (event->key() == Qt::Key_F12) {
    debug_overlay_enabled_ = !debug_overlay_enabled_;
    if (image_label_)
      image_label_->set_overlay_enabled(debug_overlay_enabled_);
    engine::Logger::instance().debug(
        "[PreviewWindow] Debug bounding-box overlay: " +
        std::string(debug_overlay_enabled_ ? "ON" : "OFF"));
    return;
  }
  QDialog::keyPressEvent(event);
}

void PreviewWindow::build_anm2_controls() {
  if (anm2_controls_)
    return;

  anm2_controls_ = new QWidget(this);
  auto *ctrl = new QVBoxLayout(anm2_controls_);
  ctrl->setContentsMargins(0, 0, 0, 0);

  // Info label: "name, X frames, Y fps"
  anm2_info_label_ = new QLabel(anm2_controls_);
  ctrl->addWidget(anm2_info_label_);

  // Scrollable animation list
  anm2_anim_list_ = new QListWidget(anm2_controls_);
  anm2_anim_list_->setMaximumHeight(120);
  ctrl->addWidget(anm2_anim_list_);

  // Speed slider row with min/max labels
  auto *speed_row = new QHBoxLayout;
  speed_row->addWidget(new QLabel(tr("Speed:"), anm2_controls_));
  auto *speed_min_label = new QLabel(tr("0.25x"), anm2_controls_);
  speed_min_label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  speed_min_label->setFixedWidth(36);
  speed_row->addWidget(speed_min_label);
  anm2_speed_slider_ = new SpeedSlider(anm2_controls_);
  speed_row->addWidget(anm2_speed_slider_, 1);
  auto *speed_max_label = new QLabel(tr("4x"), anm2_controls_);
  speed_max_label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  speed_max_label->setFixedWidth(24);
  speed_row->addWidget(speed_max_label);
  ctrl->addLayout(speed_row);

  // Play/Pause + frame counter + step buttons row
  auto *transport_row = new QHBoxLayout;
  anm2_play_btn_ = new QPushButton(tr("Play"), anm2_controls_);
  anm2_play_btn_->setCheckable(true);
  transport_row->addWidget(anm2_play_btn_);
  anm2_frame_label_ = new QLabel(tr("0/0"), anm2_controls_);
  anm2_frame_label_->setAlignment(Qt::AlignCenter);
  transport_row->addWidget(anm2_frame_label_);
  anm2_step_back_ = new QPushButton(tr("<"), anm2_controls_);
  anm2_step_back_->setToolTip(tr("Step back 1 frame"));
  transport_row->addWidget(anm2_step_back_);
  anm2_step_fwd_ = new QPushButton(tr(">"), anm2_controls_);
  anm2_step_fwd_->setToolTip(tr("Step forward 1 frame"));
  transport_row->addWidget(anm2_step_fwd_);
  ctrl->addLayout(transport_row);

  // Progress scrubber
  anm2_progress_ = new QSlider(Qt::Horizontal, anm2_controls_);
  anm2_progress_->setRange(0, 1000);
  anm2_progress_->setValue(0);
  anm2_progress_->setToolTip(tr("Drag to scrub through frames"));
  ctrl->addWidget(anm2_progress_);

  ctrl->addStretch(1);

  // Wire up signals
  connect(anm2_anim_list_, &QListWidget::currentRowChanged, this,
          [this](int row) {
            if (row >= 0 && row < static_cast<int>(anm2_states_.size()))
              switch_anm2_state(row);
          });

  connect(anm2_speed_slider_, &QSlider::valueChanged, this, [this](int val) {
    if (anm2_playing_ && !anm2_frames_.empty()) {
      double speed = val / 100.0;
      int interval = static_cast<int>(anm2_delays_[anm2_index_] / speed);
      anm2_timer_.start(std::max(interval, 1));
    }
  });

  // Magnetic snapping: when the user drags near a key speed, snap to it.
  connect(anm2_speed_slider_, &QSlider::sliderMoved, this, [this](int pos) {
    for (int snap : kSnapValues) {
      if (std::abs(pos - snap) <= kSnapThreshold) {
        QSignalBlocker blocker(anm2_speed_slider_);
        anm2_speed_slider_->setValue(snap);
        // Manually trigger the speed update since signals are blocked
        if (anm2_playing_ && !anm2_frames_.empty()) {
          double speed = snap / 100.0;
          int interval = static_cast<int>(anm2_delays_[anm2_index_] / speed);
          anm2_timer_.start(std::max(interval, 1));
        }
        break;
      }
    }
  });

  connect(anm2_play_btn_, &QPushButton::clicked, this, [this]() {
    anm2_playing_ = !anm2_playing_;
    anm2_play_btn_->setText(anm2_playing_ ? tr("Pause") : tr("Play"));
    if (anm2_playing_) {
      if (!anm2_frames_.empty()) {
        double speed = anm2_speed_slider_->value() / 100.0;
        int interval = static_cast<int>(anm2_delays_[anm2_index_] / speed);
        anm2_timer_.start(std::max(interval, 1));
      }
    } else {
      anm2_timer_.stop();
    }
  });

  connect(anm2_step_back_, &QPushButton::clicked, this, [this]() {
    if (anm2_frames_.empty())
      return;
    anm2_index_ = (anm2_index_ + anm2_frames_.size() - 1) % anm2_frames_.size();
    current_pixmap_ = anm2_frames_[anm2_index_];
    apply_zoom();
    update_anm2_ui();
  });

  connect(anm2_step_fwd_, &QPushButton::clicked, this, [this]() {
    if (anm2_frames_.empty())
      return;
    anm2_index_ = (anm2_index_ + 1) % anm2_frames_.size();
    current_pixmap_ = anm2_frames_[anm2_index_];
    apply_zoom();
    update_anm2_ui();
  });

  // Use valueChanged so clicking the scrubber also seeks (not just dragging).
  // update_anm2_ui() blocks signals on the slider to prevent feedback loops.
  connect(anm2_progress_, &QSlider::valueChanged, this, [this](int value) {
    if (anm2_frames_.empty())
      return;
    // Map 0-1000 to frame index
    int frame = static_cast<int>(value * (anm2_frames_.size() - 1) / 1000.0);
    anm2_index_ = static_cast<std::size_t>(frame);
    current_pixmap_ = anm2_frames_[anm2_index_];
    apply_zoom();
    update_anm2_ui();
  });

  // Prevent timer-driven updates while user drags the scrubber
  connect(anm2_progress_, &QSlider::sliderPressed, this,
          [this]() { anm2_timer_.stop(); });
  connect(anm2_progress_, &QSlider::sliderReleased, this, [this]() {
    if (anm2_playing_ && !anm2_frames_.empty()) {
      double speed = anm2_speed_slider_->value() / 100.0;
      int interval = static_cast<int>(anm2_delays_[anm2_index_] / speed);
      anm2_timer_.start(std::max(interval, 1));
    }
  });
}

void PreviewWindow::switch_anm2_state(int index) {
  if (index < 0 || index >= static_cast<int>(anm2_states_.size()))
    return;

  anm2_current_state_ = index;
  anm2_timer_.stop();
  anm2_playing_ = false;
  if (anm2_play_btn_)
    anm2_play_btn_->setText(tr("Play"));

  const auto &state = anm2_states_[index];
  anm2_frames_ = state.frames;
  anm2_delays_ = state.delays;
  anm2_index_ = 0;

  // Update the info label with the selected state's details.
  if (anm2_info_label_) {
    anm2_info_label_->setText(tr("%1 - %2 frames, %3 fps")
                                  .arg(state.name)
                                  .arg(static_cast<int>(state.frames.size()))
                                  .arg(state.fps));
  }

  if (!anm2_frames_.empty()) {
    current_pixmap_ = anm2_frames_.front();
    apply_zoom();
  }

  update_anm2_ui();
}

void PreviewWindow::update_anm2_ui() {
  if (anm2_frames_.empty())
    return;

  // Frame counter: "3/12"
  if (anm2_frame_label_) {
    anm2_frame_label_->setText(tr("%1/%2")
                                   .arg(static_cast<int>(anm2_index_) + 1)
                                   .arg(static_cast<int>(anm2_frames_.size())));
  }

  // Progress scrubber - block signals to avoid feedback loop with valueChanged
  if (anm2_progress_ && !anm2_progress_->isSliderDown()) {
    int pos =
        anm2_frames_.size() > 1
            ? static_cast<int>(anm2_index_ * 1000 / (anm2_frames_.size() - 1))
            : 0;
    anm2_progress_->blockSignals(true);
    anm2_progress_->setValue(pos);
    anm2_progress_->blockSignals(false);
  }
}

} // namespace ui::preview
