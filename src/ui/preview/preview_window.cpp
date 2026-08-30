#include "ui/preview/preview_window.h"
#include "ui/preview/preview_registry.h"
#include "engine/core/log/logger.h"
#include "ui/preview/preview_widget.h"

#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QVBoxLayout>

#include <algorithm>
#include <filesystem>

#include "engine/game/registry/game_features/game_feature.h"
#include "engine/game/registry/game_features/game_feature_registry.h"

namespace ui::preview {

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
         ui::preview::Registry::instance().has_preview(
             file_path.toStdString());
}

PreviewWindow::PreviewWindow(QWidget *parent) : QDialog(parent) {
  setWindowTitle(tr("Preview"));
  setMinimumSize(440, 380);
  resize(680, 520);

  auto *layout = new QVBoxLayout(this);

  auto *top = new QHBoxLayout;
  name_label_ = new QLabel(this);
  name_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  top->addWidget(name_label_, 1);
  prev_button_ = new QPushButton(tr("Previous"), this);
  next_button_ = new QPushButton(tr("Next"), this);
  top->addWidget(prev_button_);
  top->addWidget(next_button_);
  layout->addLayout(top);

  source_label_ = new QLabel(this);
  source_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  source_label_->setEnabled(false);
  layout->addWidget(source_label_);

  stack_ = new QStackedWidget(this);

  // Image page: scrollable image on a checkerboard background + zoom bar.
  image_page_ = new QWidget(this);
  auto *image_layout = new QVBoxLayout(image_page_);
  image_layout->setContentsMargins(0, 0, 0, 0);
  scroll_ = new QScrollArea(image_page_);
  scroll_->setWidgetResizable(true);
  scroll_->setAlignment(Qt::AlignCenter);
  image_label_ = new QLabel(scroll_);
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

  layout->addWidget(stack_, 1);

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
  engine::Logger::instance().debug("[PreviewWindow] show_file: file=" + file_path.toStdString() +
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
  engine::Logger::instance().debug("[PreviewWindow] reload: path=" + path.toStdString());
  engine::Logger::instance().debug("[PreviewWindow] reload: trying load_plugin_preview...");
  if (load_plugin_preview(path)) {
    engine::Logger::instance().debug("[PreviewWindow] reload: load_plugin_preview SUCCEEDED");
    return;
  }
  engine::Logger::instance().debug("[PreviewWindow] reload: trying load_image...");
  if (load_image(path)) {
    engine::Logger::instance().debug("[PreviewWindow] reload: load_image SUCCEEDED");
    return;
  }
  engine::Logger::instance().debug("[PreviewWindow] reload: trying load_anm2...");
  if (load_anm2(path)) {
    engine::Logger::instance().debug("[PreviewWindow] reload: load_anm2 SUCCEEDED");
    return;
  }
  engine::Logger::instance().debug("[PreviewWindow] reload: trying load_text...");
  if (load_text(path)) {
    engine::Logger::instance().debug("[PreviewWindow] reload: load_text SUCCEEDED");
    return;
  }
  engine::Logger::instance().debug("[PreviewWindow] reload: NO preview found, showing unsupported");
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
  stack_->setCurrentWidget(image_page_);
  apply_zoom();
  return true;
}

bool PreviewWindow::load_anm2(const QString &path) {
  if (!has_extension(path, animation_extensions()))
    return false;

  auto feature =
      ::engine::GameFeatureRegistry::instance()
          .resolve_feature<::engine::AnimationParserFeature>(game_id_);
  if (!feature)
    return false;

  std::string base_dir =
      std::filesystem::path(path.toStdString()).parent_path().string();
  auto data = feature->parse(path.toStdString(), base_dir);
  if (!data || data->frames.empty())
    return false;

  anm2_frames_.clear();
  anm2_delays_.clear();

  for (const auto &frame : data->frames) {
    QImage canvas(data->canvas_width, data->canvas_height,
                  QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);
    QPainter painter(&canvas);
    for (const auto &layer : frame.layers) {
      QImage sprite(layer.rgba_pixels.data(), layer.width, layer.height,
                    QImage::Format_RGBA8888);
      painter.drawImage(
          QPoint(static_cast<int>(layer.x), static_cast<int>(layer.y)), sprite);
    }
    painter.end();

    anm2_frames_.push_back(QPixmap::fromImage(canvas));
    anm2_delays_.push_back(frame.delay_ms);
  }

  anm2_index_ = 0;
  current_pixmap_ = anm2_frames_.front();
  stack_->setCurrentWidget(image_page_);
  apply_zoom();

  if (anm2_frames_.size() > 1) {
    anm2_timer_.start(anm2_delays_.front());
  }
  return true;
}

void PreviewWindow::on_anm2_frame_timeout() {
  if (anm2_frames_.empty())
    return;
  anm2_index_ = (anm2_index_ + 1) % anm2_frames_.size();
  current_pixmap_ = anm2_frames_[anm2_index_];
  apply_zoom();
  anm2_timer_.setInterval(anm2_delays_[anm2_index_]);
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
  stack_->setCurrentWidget(text_view_);
  return true;
}

bool PreviewWindow::load_plugin_preview(const QString &path) {
  engine::Logger::instance().debug("[PreviewWindow] load_plugin_preview: path=" + path.toStdString());
  // Drop any previously embedded plugin widget before resolving the new file so
  // a fallback to a built-in preview doesn't leave a stale widget behind.
  if (plugin_widget_) {
    plugin_layout_->removeWidget(plugin_widget_);
    delete plugin_widget_;
    plugin_widget_ = nullptr;
  }

  // Ask the v2 IPluginPreview registry for this file's extension. A plugin that
  // claimed the extension returns a QWidget* (as opaque void*); we embed it.
  engine::Logger::instance().debug("[PreviewWindow] load_plugin_preview: calling Registry::create_preview");
  void *w = ui::preview::Registry::instance().create_preview(
      path.toStdString());
  engine::Logger::instance().debug("[PreviewWindow] load_plugin_preview: create_preview returned w=" +
                           std::to_string(reinterpret_cast<uintptr_t>(w)));
  if (!w) {
    engine::Logger::instance().debug("[PreviewWindow] load_plugin_preview: no plugin preview, returning false");
    return false;
  }

  plugin_widget_ = reinterpret_cast<QWidget *>(w);
  plugin_layout_->addWidget(plugin_widget_);
  stack_->setCurrentWidget(plugin_page_);
  return true;
}

void PreviewWindow::show_unsupported() {
  engine::Logger::instance().debug("[PreviewWindow] show_unsupported: displaying unsupported message");
  anm2_timer_.stop();
  anm2_frames_.clear();
  anm2_delays_.clear();
  anm2_index_ = 0;
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
  image_label_->setPixmap(current_pixmap_.scaled(target, Qt::KeepAspectRatio,
                                                 Qt::SmoothTransformation));
  if (!fit_)
    zoom_label_->setText(QString::number(qRound(zoom_ * 100.0)) + "%");
  else
    zoom_label_->setText(QStringLiteral("100%"));
}

void PreviewWindow::resizeEvent(QResizeEvent *event) {
  QDialog::resizeEvent(event);
  if (fit_ && stack_ && stack_->currentWidget() == image_page_)
    apply_zoom();
}

} // namespace ui::preview
