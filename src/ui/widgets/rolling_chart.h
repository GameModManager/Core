#pragma once

#include <QWidget>
#include <deque>

// When Qt6 Charts is available the widget renders via QChart/QChartView/
// QLineSeries. The headers live under <QtCharts/QChart> etc. (module
// include path) and are pulled in only from the .cpp under the
// GMM_HAS_QTCHARTS guard.
#ifdef GMM_HAS_QTCHARTS
class QChart;
class QChartView;
class QLineSeries;
class QValueAxis;
#endif

// Lightweight rolling 60s chart for the DebugWindow. Each chart is a fixed
// 60-second window that scrolls left as time passes; the X axis spans the
// last 60 seconds and the Y axis is auto-scaled to the sample range (with a
// minimum floor for percentage-style series).
//
// Tech: Qt6 Charts (QChart/QChartView/QLineSeries) when Qt6::Charts is
// available; otherwise a hand-drawn QPainter polyline with grid lines. The
// QPainter path renders identically to the QtCharts one and is what the
// fallback build (CI without qt6-charts) shows. Colors come from QPalette
// (highlight for the line, windowText for axis labels, alternateBase for
// the grid) - never hardcoded - so the charts respect the theme.
//
// Usage (single series):
//   auto* chart = new ui::RollingChartWidget();
//   chart->set_y_range(0.0, 100.0);
//   chart->set_y_label("%");
//   chart->set_title("CPU");
//   // Each tick:
//   chart->push_sample(value);
//   chart->set_legend("user");
//
// Usage (two series, e.g. Read/Write or RX/TX):
//   auto* chart = new ui::RollingChartWidget();
//   chart->set_clamp_negative(true); // Disk/Net cannot go below 0
//   chart->set_legend("Read");
//   chart->set_legend2("Write");
//   // Each tick:
//   chart->push_samples(read_value, write_value);
namespace ui {

class RollingChartWidget : public QWidget {
  Q_OBJECT
public:
  explicit RollingChartWidget(QWidget *parent = nullptr);
  ~RollingChartWidget() override;

  // Append a new sample to the rolling window. If this is the first
  // sample, the window seeds with that value repeated to history length
  // so the line starts as a flat baseline.
  void push_sample(double value);

  // Two-series variant: pushes (a, b) into the rolling window in
  // lock-step. Both series share the same X axis (the 1 Hz sample
  // clock). Use for Read/Write, RX/TX, or any two non-negative
  // quantities that should be drawn on the same chart. The second
  // series is only drawn when push_samples() has been called; legacy
  // single-series callers (CPU, RAM, heap, jitter) keep working.
  void push_samples(double a, double b);

  // Set fixed Y axis range (use 0,0 for auto-scale).
  void set_y_range(double min, double max);

  // Set the Y axis label (e.g. "%", "MiB", "KiB/s"). Empty hides it.
  void set_y_label(const QString &label);

  // Set chart title (shown at top via QChart title, or as overlay text in
  // the QPainter fallback). Empty hides it.
  void set_title(const QString &title);

  // Set legend label for the first series (shown in tooltip / hover).
  // Currently used only by the QPainter fallback.
  void set_legend(const QString &legend);
  // Second-series label (paired with push_samples(a, b)). Ignored when
  // only one series is being pushed.
  void set_legend2(const QString &legend);

  // When true, the auto-scaled Y axis clamps the lower bound to 0. Use
  // for any quantity that cannot physically be negative (Disk IO,
  // Network IO, RSS, heap, CPU%). Has no effect on a fixed range set
  // via set_y_range(); only the auto-scaler respects the clamp. The
  // signed jitter chart must leave this off so the +/- 50 ms range
  // works.
  void set_clamp_negative(bool clamp);
  bool clamp_negative() const { return clamp_negative_; }

  // Clear all samples (and history) - useful when the active instance
  // changes and the old chart history no longer makes sense.
  void reset();

  // Cap on the rolling window (60 seconds at 1 Hz = 60 samples).
  static constexpr std::size_t kCapacity = 60;

private:
#ifdef GMM_HAS_QTCHARTS
  void rebuild_qtchart();
  void apply_qtchart_axis();
  // Members are QChart etc. (forward-declared above in the
  // QtCharts namespace block). Qualifying with the namespace here is
  // mandatory: the Q_OBJECT moc-generated code references them in the
  // namespace-qualified form, and unqualified bare `QChart *` would not
  // match the real type when Qt6::Charts is linked.
  QChart *chart_ = nullptr;
  QChartView *chart_view_ = nullptr;
  QLineSeries *series_ = nullptr;
  QLineSeries *series2_ = nullptr;
  QValueAxis *axis_x_ = nullptr;
  QValueAxis *axis_y_ = nullptr;
  double y_min_ = 0.0;
  double y_max_ = 0.0;
  bool y_range_set_ = false;
  QString y_label_;
  QString title_;
  QString legend_;
  QString legend2_;
  bool clamp_negative_ = false;
  std::deque<double> samples_;  // back() = newest; size() <= kCapacity
  std::deque<double> samples2_; // paired with samples_; same length
#else
  void paintEvent(QPaintEvent *event) override;
  QString title_;
  QString y_label_;
  QString legend_;
  QString legend2_;
  double y_min_ = 0.0;
  double y_max_ = 0.0;
  bool y_range_set_ = false;
  bool clamp_negative_ = false;
  std::deque<double> samples_;
  std::deque<double> samples2_;
#endif
};

} // namespace ui