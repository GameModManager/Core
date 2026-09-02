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
// Usage:
//   auto* chart = new ui::RollingChartWidget();
//   chart->set_y_range(0.0, 100.0);
//   chart->set_y_label("%");
//   chart->set_title("CPU");
//   // Each tick:
//   chart->push_sample(value);
//   chart->set_legend("user");
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

  // Set fixed Y axis range (use 0,0 for auto-scale).
  void set_y_range(double min, double max);

  // Set the Y axis label (e.g. "%", "MiB", "KiB/s"). Empty hides it.
  void set_y_label(const QString &label);

  // Set chart title (shown at top via QChart title, or as overlay text in
  // the QPainter fallback). Empty hides it.
  void set_title(const QString &title);

  // Set legend label for the single series (shown in tooltip / hover).
  // Currently used only by the QPainter fallback.
  void set_legend(const QString &legend);

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
  QValueAxis *axis_x_ = nullptr;
  QValueAxis *axis_y_ = nullptr;
  double y_min_ = 0.0;
  double y_max_ = 0.0;
  bool y_range_set_ = false;
  QString y_label_;
  QString title_;
  QString legend_;
  std::deque<double> samples_; // back() = newest; size() <= kCapacity
#else
  void paintEvent(QPaintEvent *event) override;
  QString title_;
  QString y_label_;
  QString legend_;
  double y_min_ = 0.0;
  double y_max_ = 0.0;
  bool y_range_set_ = false;
  std::deque<double> samples_;
#endif
};

} // namespace ui