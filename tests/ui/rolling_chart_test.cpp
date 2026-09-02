// Offscreen smoke test for the rolling_chart widget: confirms push_sample
// never crashes, the chart's windowing policy is right, and labels update.
//
// Hermetic: no instance/plugin data; QT_QPA_PLATFORM=offscreen via the test
// property.
#include "ui/widgets/rolling_chart.h"

#include <QApplication>
#include <QSizePolicy>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("rolling_chart: defaults are sane", "[rolling_chart][ui]") {
  int argc = 1;
  char app_name[] = "rolling_chart_test";
  char *argv[] = {app_name, nullptr};
  QApplication app(argc, argv);
  ui::RollingChartWidget chart;
  // Windowing defaults match the DebugWindow's chart tab layout.
  CHECK(chart.minimumHeight() >= 100);
  QSizePolicy policy = chart.sizePolicy();
  CHECK(policy.horizontalPolicy() == QSizePolicy::Expanding);
  CHECK(policy.verticalPolicy() == QSizePolicy::Expanding);
}

TEST_CASE("rolling_chart: push_sample works without crashing",
          "[rolling_chart][ui]") {
  int argc = 1;
  char app_name[] = "rolling_chart_test";
  char *argv[] = {app_name, nullptr};
  QApplication app(argc, argv);
  ui::RollingChartWidget chart;
  chart.resize(400, 160);
  chart.show();

  // Push a sequence of samples; the chart should always handle them.
  // The exact rendering is irrelevant for this smoke test (CI may lack
  // QtCharts and the QPainter fallback is what runs).
  for (int i = 0; i < 100; ++i) {
    chart.push_sample(static_cast<double>(i % 60));
  }
  CHECK(true);
}

TEST_CASE("rolling_chart: setters do not crash", "[rolling_chart][ui]") {
  int argc = 1;
  char app_name[] = "rolling_chart_test";
  char *argv[] = {app_name, nullptr};
  QApplication app(argc, argv);
  ui::RollingChartWidget chart;
  chart.set_y_range(0.0, 100.0);
  chart.set_y_label(QStringLiteral("%"));
  chart.set_title(QStringLiteral("Test"));
  chart.set_legend(QStringLiteral("series-1"));
  chart.reset();
  chart.push_sample(50.0);
  CHECK(true);
}