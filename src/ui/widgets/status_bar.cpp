#include "ui/widgets/status_bar.h"

#include "engine/source/registry.h"
#include "engine/trace/trace_recorder.h"
#include "ui/settings/settings.h"

#include <QCoreApplication>
#include <QFrame>
#include <QProgressBar>
#include <QToolButton>

namespace ui {

namespace {
  // Well-known flow ids shown in the pipeline indicator.
  const char *kFlowIds[]    = {"launch", "install", "sort"};
  const char *kFlowTitles[] = {"Launch", "Install", "Sort"};

  // How long a transient status message holds the left-hand label before the
  // context label takes it back.
  constexpr int kStatusTimeoutMs = 5000;

  // The game's download_sources knowledge names its sources for display
  // ("Nexus Mods"), which is not the provider's source_type ("nexus"). Compare
  // a normalized form of either, so the rule below can stay "does this source
  // meter a budget" instead of becoming a per-source name list.
  QString normalized(const QString &text) {
    QString out;
    for (const auto c : text) {
      if (c.isLetterOrNumber())
        out.append(c.toLower());
    }
    return out;
  }

  engine::Source::Interface *provider_for(const QString &source) {
    const auto key = normalized(source);
    for (auto *provider : engine::Source::Registry::instance().providers()) {
      if (normalized(QString::fromStdString(provider->display_name())) == key ||
          normalized(QString::fromStdString(provider->source_type())) == key)
        return provider;
    }
    return nullptr;
  }
}  // namespace

QString context_label_text(const QString &game, const QString &instance,
                           const QString &profile) {
  return QString("%1 - %2 - %3")
      .arg(game.isEmpty() ? QCoreApplication::translate("ui::StatusBar", "Unknown game")
                          : game)
      .arg(instance.isEmpty() ? QStringLiteral("?") : instance)
      .arg(profile.isEmpty() ? QStringLiteral("?") : profile);
}

StatusBar::StatusBar(QWidget *parent) : QWidget(parent) {
  layout_ = new QHBoxLayout(this);
  layout_->setContentsMargins(6, 2, 6, 2);
  layout_->setSpacing(12);

  // Left: the context label is the resting state of this label; a transient
  // status borrows it until it times out. Nothing is loaded yet at
  // construction, so the resting text is the idle one.
  context_ = tr("Ready");
  status_label_ = new QLabel(context_, this);
  layout_->addWidget(status_label_);

  layout_->addStretch();

  // MO2 statusbar.cpp:31-37 and :45 (setProgress(-1) at startup): a bar that
  // stays hidden until an operation reports progress, so the resting bar never
  // claims work that is not running.
  progress_ = new QProgressBar(this);
  progress_->setObjectName("statusProgress");
  progress_->setTextVisible(true);
  progress_->setRange(0, 100);
  progress_->setMaximumWidth(300);
  progress_->setVisible(false);
  layout_->addWidget(progress_);

  // Pipeline activity indicator - click to open the pipeline window
  pipeline_button_ = new QToolButton(this);
  pipeline_button_->setObjectName("pipelineIndicator");
  pipeline_button_->setText("Pipeline: idle");
  pipeline_button_->setToolTip("Workflow pipeline - click to open");
  pipeline_button_->setAutoRaise(true);
  layout_->addWidget(pipeline_button_);
  connect(pipeline_button_, &QToolButton::clicked, this, &StatusBar::pipeline_clicked);

  // Restores the context label once a transient status has had its turn. The
  // interval is set once here rather than passed to start(), so restarting
  // the timer cannot change how long a message is allowed to hold the label.
  status_timer_ = new QTimer(this);
  status_timer_->setObjectName("statusTimeout");
  status_timer_->setSingleShot(true);
  status_timer_->setInterval(kStatusTimeoutMs);
  connect(status_timer_, &QTimer::timeout, this,
          [this] { status_label_->setText(context_); });

  pipeline_timer_ = new QTimer(this);
  connect(pipeline_timer_, &QTimer::timeout, this,
          &StatusBar::refresh_pipeline_indicator);
  pipeline_timer_->start(2000);

  refresh_pipeline_indicator();
}

void StatusBar::set_context(const QString &text) {
  context_ = text;
  // A transient status owns the label until it expires, so leave it be.
  if (!status_timer_->isActive())
    status_label_->setText(context_);
}

void StatusBar::set_status(const QString &text) {
  status_label_->setText(text);
  status_timer_->start();
}

// MO2 StatusBar::setProgress (statusbar.cpp:58-70): a percent outside 0-99
// means "no operation running", so the bar goes away and the label is handed
// back rather than left reading "Loading..." forever.
void StatusBar::set_progress(int percent) {
  const bool showing = percent >= 0 && percent < 100;
  progress_->setVisible(showing);
  if (!showing) {
    // clearMessage() in MO2: the transient status is over.
    status_timer_->stop();
    status_label_->setText(context_);
    return;
  }
  progress_->setValue(percent);
  status_label_->setText(tr("Loading..."));
  status_timer_->stop();
}

int StatusBar::progress() const {
  // isHidden(), not isVisible(): the question is whether set_progress() put
  // the bar away, which is true regardless of whether this bar is on screen.
  return progress_->isHidden() ? -1 : progress_->value();
}

QStringList StatusBar::source_names() const {
  return source_labels_by_name_.keys();
}

QString StatusBar::source_text(const QString &name) const {
  auto it = source_labels_by_name_.constFind(name);
  return it == source_labels_by_name_.constEnd() ? QString() : it.value()->text();
}

void StatusBar::set_sources(const QStringList &sources) {
  clear_source_labels();

  // Only a source that meters a request budget gets a label. A source with no
  // meter - one with no API and no cooldown we enforce - would otherwise sit
  // there reading "--" forever, advertising a number we never compute.
  //
  // Settings > Sources > "Hide API Request Counter" (default off) drops the
  // whole group: the readouts ARE the per-source request counters, and hiding
  // them also drops the divider, so an empty bar is never left with a bare "|".
  QStringList metered;
  if (!Settings::instance().hide_api_counter()) {
    for (const auto &source : sources) {
      auto *provider = provider_for(source);
      if (provider != nullptr && provider->rate_limit_readout().metered)
        metered.append(source);
    }
  }

  // The separator divides the source readouts from the rest of the bar, so it
  // is built with the first readout. Adding it before any readout existed
  // left a bare "|" on the bar whenever no instance was loaded.
  if (!metered.isEmpty()) {
    separator_ = new QFrame();
    separator_->setFrameShape(QFrame::VLine);
    separator_->setFrameShadow(QFrame::Sunken);
    layout_->addWidget(separator_);
  }

  for (const auto &source : metered) {
    auto *label = new QLabel(this);
    label->setStyleSheet("color: gray;");
    layout_->addWidget(label);
    source_labels_.append(label);
    source_labels_by_name_.insert(source, label);
  }

  refresh_source_meters();
}

void StatusBar::clear_source_labels() {
  for (auto *label : source_labels_) {
    layout_->removeWidget(label);
    label->deleteLater();
  }
  source_labels_.clear();
  source_labels_by_name_.clear();

  if (separator_ != nullptr) {
    layout_->removeWidget(separator_);
    separator_->deleteLater();
    separator_ = nullptr;
  }
}

void StatusBar::refresh_source_meters() {
  for (auto it = source_labels_by_name_.cbegin(); it != source_labels_by_name_.cend();
       ++it) {
    auto *provider = provider_for(it.key());
    if (provider == nullptr)
      continue;
    const auto meter = provider->rate_limit_readout();
    it.value()->setText(it.key() + ": " + QString::fromStdString(meter.readout));
  }
}

void StatusBar::refresh_pipeline_indicator() {
  refresh_source_meters();

  auto &trace = engine::TraceRecorder::instance();
  QString text;
  bool any_running = false;

  for (int i = 0; i < 3; ++i) {
    auto snap = trace.snapshot(kFlowIds[i]);
    if (snap && snap->running) {
      if (!any_running) {
        text        = QString("Pipeline: %1 running…").arg(kFlowTitles[i]);
        any_running = true;
      }
    }
  }
  if (!any_running) {
    text = "Pipeline: idle";
  }
  pipeline_button_->setText(text);
}

}  // namespace ui
