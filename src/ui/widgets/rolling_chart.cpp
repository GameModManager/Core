#include "ui/widgets/rolling_chart.h"

#include <QApplication>
#include <QFont>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPen>
#include <QVBoxLayout>

#ifdef GMM_HAS_QTCHARTS
// Canonical Qt6 path: include the per-class headers under QtCharts/.
#include <QtCharts/QChart>
#include <QtCharts/QChartView>
#include <QtCharts/QLineSeries>
#include <QtCharts/QValueAxis>
#include <QtMath>
#endif

namespace ui {

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

void RollingChartWidget::reset() {
  samples_.clear();
#ifdef GMM_HAS_QTCHARTS
  if (series_)
    series_->replace(QList<QPointF>{});
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
      // Auto-scale Y to the visible sample range with a small floor so
      // a flat-at-zero trace still draws (avoid axis range 0..0).
      double lo = samples_.front();
      double hi = samples_.front();
      for (double v : samples_) {
        if (v < lo)
          lo = v;
        if (v > hi)
          hi = v;
      }
      if (qFuzzyCompare(lo, hi)) {
        lo -= 1.0;
        hi += 1.0;
      } else {
        const double pad = (hi - lo) * 0.1;
        lo -= pad;
        hi += pad;
      }
      if (axis_y_) {
        axis_y_->setRange(lo, hi);
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

  axis_x_ = new QValueAxis(chart_);
  axis_x_->setRange(0, kCapacity);
  axis_x_->setLabelFormat("%d");
  axis_x_->setTitleText(QStringLiteral("s ago"));
  chart_->addAxis(axis_x_, Qt::AlignBottom);
  series_->attachAxis(axis_x_);

  axis_y_ = new QValueAxis(chart_);
  axis_y_->setRange(0, 100);
  chart_->addAxis(axis_y_, Qt::AlignLeft);
  series_->attachAxis(axis_y_);

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

  // Y range (fixed or auto). When auto, derive from samples with padding.
  double lo = y_min_;
  double hi = y_max_;
  if (!y_range_set_) {
    if (samples_.empty()) {
      lo = 0.0;
      hi = 1.0;
    } else {
      lo = samples_.front();
      hi = samples_.front();
      for (double v : samples_) {
        if (v < lo)
          lo = v;
        if (hi < v)
          hi = v;
      }
      if (qFuzzyCompare(1.0 + lo, 1.0 + hi)) {
        lo -= 1.0;
        hi += 1.0;
      } else {
        const double pad = (hi - lo) * 0.1;
        lo -= pad;
        hi += pad;
      }
    }
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

  // Plot polyline
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

  QPainterPath path;
  bool first = true;
  std::size_t i = 0;
  for (double v : samples_) {
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
  p.setPen(QPen(pal.highlight().color(), 1.5));
  p.drawPath(path);
}

#endif // GMM_HAS_QTCHARTS

} // namespace ui