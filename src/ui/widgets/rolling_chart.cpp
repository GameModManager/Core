#include "ui/widgets/rolling_chart.h"

#include <QApplication>
#include <QFont>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPen>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>

#ifdef GMM_HAS_QTCHARTS
// Canonical Qt6 path: include the per-class headers under QtCharts/.
#include <QtCharts/QChart>
#include <QtCharts/QChartView>
#include <QtCharts/QLineSeries>
#include <QtCharts/QValueAxis>
#include <QtMath>
#endif

namespace ui {

namespace {

// Compute the auto-scaled Y range from one or two sample sequences.
// When clamp_negative is true the lower bound is clamped to 0 (for
// quantities that cannot physically be negative: Disk IO, Network IO,
// RSS, heap, CPU%). Both series are folded into one range so the
// chart always shows the larger of the two.
//
// Idle-trace special case: when all samples collapse to a single
// value (including 0) we still draw a useful range. The clamp-
// negative path picks [0, 1] so the trace hugs the bottom; the
// un-clamped path (jitter) keeps the +/-1 floor so a steady-state
// trace centres.
struct AutoRange {
  double lo;
  double hi;
};
AutoRange auto_range_from(const std::deque<double> &a,
                          const std::deque<double> &b, bool clamp_negative) {
  AutoRange r{0.0, 1.0};
  if (a.empty() && b.empty())
    return r;
  double lo = std::numeric_limits<double>::infinity();
  double hi = -std::numeric_limits<double>::infinity();
  for (double v : a) {
    if (v < lo)
      lo = v;
    if (v > hi)
      hi = v;
  }
  for (double v : b) {
    if (v < lo)
      lo = v;
    if (v > hi)
      hi = v;
  }
  if (!std::isfinite(lo) || !std::isfinite(hi))
    return r;
  if (qFuzzyCompare(1.0 + lo, 1.0 + hi)) {
    // Flat trace: keep a small floor so the line still draws. When
    // clamp_negative is set we anchor the floor at 0 so an idle
    // chart never shows negative values.
    if (clamp_negative && lo <= 0.0) {
      r.lo = 0.0;
      r.hi = std::max(hi, 1.0);
    } else {
      r.lo = lo - 1.0;
      r.hi = hi + 1.0;
    }
    return r;
  }
  // Pad both sides by 10% of the range, then clamp the lower bound
  // when requested. The clamp is the bug fix: previously lo -= pad
  // pushed the axis below 0 even when every sample was >= 0, and
  // qFuzzyCompare + lo -= 1.0 made the idle chart show [-1, +1].
  const double pad = (hi - lo) * 0.1;
  r.lo = lo - pad;
  r.hi = hi + pad;
  if (clamp_negative && r.lo < 0.0)
    r.lo = 0.0;
  return r;
}

} // namespace

RollingChartWidget::RollingChartWidget(QWidget *parent) : QWidget(parent) {
  setMinimumHeight(140);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
#ifdef GMM_HAS_QTCHARTS
  rebuild_qtchart();
#else
  setAutoFillBackground(false);
#endif
}

RollingChartWidget::~RollingChartWidget() = default;

void RollingChartWidget::set_y_range(double min, double max) {
  y_range_set_ = true;
  y_min_ = min;
  y_max_ = max;
#ifdef GMM_HAS_QTCHARTS
  apply_qtchart_axis();
#endif
  this->update();
}

void RollingChartWidget::set_y_label(const QString &label) {
  y_label_ = label;
#ifdef GMM_HAS_QTCHARTS
  if (axis_y_)
    axis_y_->setTitleText(label);
#endif
  this->update();
}

void RollingChartWidget::set_title(const QString &title) {
  title_ = title;
#ifdef GMM_HAS_QTCHARTS
  if (chart_)
    chart_->setTitle(title);
#endif
  this->update();
}

void RollingChartWidget::set_legend(const QString &legend) {
  legend_ = legend;
  this->update();
}

void RollingChartWidget::set_legend2(const QString &legend) {
  legend2_ = legend;
  this->update();
}

void RollingChartWidget::set_clamp_negative(bool clamp) {
  clamp_negative_ = clamp;
  // Force an immediate axis re-evaluation on the next push so the
  // toggle reflects in the rendered chart without waiting for a tick.
  this->update();
}

void RollingChartWidget::reset() {
  samples_.clear();
  samples2_.clear();
#ifdef GMM_HAS_QTCHARTS
  if (series_)
    series_->replace(QList<QPointF>{});
  if (series2_)
    series2_->replace(QList<QPointF>{});
#endif
  this->update();
}

void RollingChartWidget::push_sample(double value) {
  if (samples_.empty()) {
    // Seed the window with the current value so the line starts as a
    // flat baseline. Without this the chart jumps on tick 2 because
    // the rest of the deque is uninitialized.
    samples_.assign(kCapacity, value);
  } else {
    if (samples_.size() >= kCapacity)
      samples_.pop_front();
    samples_.push_back(value);
  }

#ifdef GMM_HAS_QTCHARTS
  if (series_) {
    QList<QPointF> points;
    points.reserve(static_cast<int>(samples_.size()));
    const double n = static_cast<double>(samples_.size());
    const int cap = static_cast<int>(kCapacity);
    // X axis is "seconds ago" with the newest sample at X=0. The visible
    // window always spans the full kCapacity so the chart looks stable.
    // We anchor the latest sample to the right edge: index i maps to
    //   x = cap - n + i
    // so the leftmost point is at x = cap - n and the rightmost at x = cap.
    const double base_x = cap - n;
    int i = 0;
    for (double v : samples_) {
      points.append(QPointF(base_x + i, v));
      ++i;
    }
    series_->replace(points);
    if (!y_range_set_) {
      // Auto-scale Y to the visible sample range. The helper honours
      // clamp_negative_ so non-negative quantities (Disk/Network IO,
      // RSS, heap, CPU%) cannot end up with a negative lower bound.
      auto r = auto_range_from(samples_, samples2_, clamp_negative_);
      if (axis_y_) {
        axis_y_->setRange(r.lo, r.hi);
      }
    }
  }
#endif
  this->update();
}

void RollingChartWidget::push_samples(double a, double b) {
  // Two-series mode: keep both deques in lock-step with the same X.
  // The single-series API is still supported for legacy callers
  // (CPU, RAM, heap, jitter) - this overload is opt-in by use.
  auto append = [this](std::deque<double> &q, double v) {
    if (q.empty()) {
      q.assign(kCapacity, v);
    } else {
      if (q.size() >= kCapacity)
        q.pop_front();
      q.push_back(v);
    }
  };
  append(samples_, a);
  append(samples2_, b);

#ifdef GMM_HAS_QTCHARTS
  if (series_ && series2_) {
    QList<QPointF> points_a;
    QList<QPointF> points_b;
    points_a.reserve(static_cast<int>(samples_.size()));
    points_b.reserve(static_cast<int>(samples2_.size()));
    const double n = static_cast<double>(samples_.size());
    const int cap = static_cast<int>(kCapacity);
    const double base_x = cap - n;
    int i = 0;
    auto it_b = samples2_.begin();
    for (double v : samples_) {
      points_a.append(QPointF(base_x + i, v));
      if (it_b != samples2_.end()) {
        points_b.append(QPointF(base_x + i, *it_b));
        ++it_b;
      }
      ++i;
    }
    series_->replace(points_a);
    series2_->replace(points_b);
    if (!y_range_set_) {
      auto r = auto_range_from(samples_, samples2_, clamp_negative_);
      if (axis_y_) {
        axis_y_->setRange(r.lo, r.hi);
      }
    }
  }
#endif
  this->update();
}

#ifdef GMM_HAS_QTCHARTS

void RollingChartWidget::rebuild_qtchart() {
  chart_ = new QChart();
  chart_->legend()->hide();
  chart_->setBackgroundRoundness(0);
  chart_->setMargins(QMargins(4, 4, 4, 4));

  series_ = new QLineSeries(chart_);
  chart_->addSeries(series_);
  // Second series shares the X / Y axes with the first; it is
  // created up front (rather than lazily on the first push_samples
  // call) so the legend / colours / axis attachments are stable and
  // the chart does not flicker when the caller switches modes.
  series2_ = new QLineSeries(chart_);
  chart_->addSeries(series2_);

  axis_x_ = new QValueAxis(chart_);
  axis_x_->setRange(0, kCapacity);
  axis_x_->setLabelFormat("%d");
  axis_x_->setTitleText(QStringLiteral("s ago"));
  chart_->addAxis(axis_x_, Qt::AlignBottom);
  series_->attachAxis(axis_x_);
  series2_->attachAxis(axis_x_);

  axis_y_ = new QValueAxis(chart_);
  axis_y_->setRange(0, 100);
  chart_->addAxis(axis_y_, Qt::AlignLeft);
  series_->attachAxis(axis_y_);
  series2_->attachAxis(axis_y_);

  // Palette colors: chart background = window, plot area = base, line =
  // highlight, axis labels = windowText. This keeps the chart themed by
  // QPalette and tracks the rest of the app's dark/light style.
  QPalette p = palette();
  chart_->setBackgroundBrush(p.window());
  chart_->setPlotAreaBackgroundBrush(p.base());
  chart_->setPlotAreaBackgroundVisible(true);
  chart_->setTitleBrush(p.windowText());
  QPen axis_pen(p.mid().color());
  axis_pen.setWidthF(1.0);
  axis_x_->setLinePen(axis_pen);
  axis_y_->setLinePen(axis_pen);
  axis_x_->setLabelsBrush(p.windowText());
  axis_y_->setLabelsBrush(p.windowText());
  axis_x_->setTitleBrush(p.windowText());
  axis_y_->setTitleBrush(p.windowText());
  axis_x_->setGridLinePen(QPen(p.alternateBase().color(), 1, Qt::DotLine));
  axis_y_->setGridLinePen(QPen(p.alternateBase().color(), 1, Qt::DotLine));

  QPen series_pen(p.highlight().color());
  series_pen.setWidthF(1.5);
  series_->setPen(series_pen);
  series_->setColor(p.highlight().color());

  // Primary series uses highlight (the standard "main metric" colour).
  // Secondary series uses Link - a palette role that contrasts with
  // highlight in both light and dark themes (blue vs the typical
  // orange highlight), so the two flows are visually distinguishable
  // without hardcoding RGB.
  QPen series2_pen(p.link().color());
  series2_pen.setWidthF(1.5);
  series2_->setPen(series2_pen);
  series2_->setColor(p.link().color());

  chart_view_ = new QChartView(chart_, this);
  chart_view_->setRenderHint(QPainter::Antialiasing);

  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(chart_view_);
}

void RollingChartWidget::apply_qtchart_axis() {
  if (axis_y_ && y_range_set_) {
    axis_y_->setRange(y_min_, y_max_);
  }
}

#else // !GMM_HAS_QTCHARTS - QPainter fallback

void RollingChartWidget::paintEvent(QPaintEvent * /*event*/) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);

  const QPalette pal = palette();

  // Background
  p.fillRect(rect(), pal.base());

  // Layout: title row + plot area.
  const int margin = 6;
  QFont title_font = p.font();
  title_font.setBold(true);
  p.setFont(title_font);
  QFontMetrics title_fm(title_font);
  const int title_h = title_.isEmpty() ? 0 : title_fm.height() + 2;

  QRect plot_area(rect().left() + margin, rect().top() + margin + title_h,
                  rect().width() - 2 * margin,
                  rect().height() - 2 * margin - title_h);
  if (plot_area.width() < 10 || plot_area.height() < 10)
    return;

  // Title
  if (!title_.isEmpty()) {
    p.setPen(pal.windowText().color());
    p.drawText(plot_area, Qt::AlignLeft | Qt::AlignTop, title_);
  }

  // Y range (fixed or auto). When auto, derive from samples with
  // padding and honour clamp_negative so non-negative metrics never
  // draw below 0.
  double lo = y_min_;
  double hi = y_max_;
  if (!y_range_set_) {
    auto r = auto_range_from(samples_, samples2_, clamp_negative_);
    lo = r.lo;
    hi = r.hi;
  }

  // Grid + axis box
  p.setPen(QPen(pal.alternateBase().color(), 1, Qt::DotLine));
  const int grid_lines = 4;
  for (int i = 1; i < grid_lines; ++i) {
    const int y = plot_area.top() + (plot_area.height() * i) / grid_lines;
    p.drawLine(plot_area.left(), y, plot_area.right(), y);
  }
  for (int i = 1; i < 6; ++i) {
    const int x = plot_area.left() + (plot_area.width() * i) / 6;
    p.drawLine(x, plot_area.top(), x, plot_area.bottom());
  }

  // Y label (top-left, rotated would be ideal but vertical text is tricky
  // on every platform; print horizontally in the title row instead).
  if (!y_label_.isEmpty()) {
    QFont small_font = title_font;
    small_font.setBold(false);
    small_font.setPointSizeF(title_font.pointSizeF() * 0.85);
    p.setFont(small_font);
    p.setPen(pal.windowText().color());
    p.drawText(plot_area.adjusted(plot_area.width() - 60, 0, 0, 0),
               Qt::AlignTop | Qt::AlignRight, y_label_);
    p.setFont(title_font);
  }

  // Plot polylines. Both series share the same X scale (the rolling
  // window) and the same Y range (auto-scaled from the union of
  // both).
  if (samples_.empty())
    return;
  const double range = hi - lo;
  auto to_y = [&](double v) {
    if (range <= 0.0)
      return plot_area.top() + plot_area.height() / 2.0;
    const double t = (v - lo) / range;
    return plot_area.bottom() - t * plot_area.height();
  };
  auto to_x = [&](std::size_t i) {
    // Anchor newest sample at right edge, oldest at right-edge - n.
    const double n = static_cast<double>(samples_.size());
    const double cap = static_cast<double>(kCapacity);
    const double t = (cap - n + static_cast<double>(i)) / cap;
    return plot_area.left() + t * plot_area.width();
  };

  auto draw_polyline = [&](const std::deque<double> &q, const QColor &color) {
    QPainterPath path;
    bool first = true;
    std::size_t i = 0;
    for (double v : q) {
      const double x = to_x(i);
      const double y = to_y(v);
      if (first) {
        path.moveTo(x, y);
        first = false;
      } else {
        path.lineTo(x, y);
      }
      ++i;
    }
    p.setPen(QPen(color, 1.5));
    p.drawPath(path);
  };

  draw_polyline(samples_, pal.highlight().color());
  if (!samples2_.empty()) {
    draw_polyline(samples2_, pal.link().color());
  }
}

#endif // GMM_HAS_QTCHARTS

} // namespace ui