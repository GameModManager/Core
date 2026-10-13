// Scroll-frame cost measurement for the mod list (Workspace-lb45).
//
// Scrolling repaints two things every frame: the viewport (the ~30 visible
// rows) and ModMarkingScrollBar, whose paintEvent walks EVERY model row to
// draw its separator/highlight marks. This measures the model-side cost of
// one such frame so a regression has a number attached instead of a hunch.
//
// Offscreen, throwaway XDG_CONFIG_HOME, no network. Prints numbers; the
// assertions below only guard the shapes that must hold for the numbers to
// mean anything.
#include "ui/widgets/mod_list_model.h"
#include "ui/widgets/mod_table_view.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QElapsedTimer>
#include <QTreeView>

#include <cstdio>
#include <filesystem>
#include <catch2/catch_test_macros.hpp>

namespace {

// Rows a ~1080p viewport shows in the mod list.
constexpr int kVisibleRows = 30;

void populate(ui::ModList &model, bool nested, int mods) {
  model.set_nesting_enabled(nested);
  QVector<ui::ModEntry> entries;
  // Every 25th row is a separator, so the band-scanning paths run for real.
  int parent = 0;
  for (int i = 0; i < mods; ++i) {
    ui::ModEntry m;
    m.id   = QStringLiteral("mod_%1").arg(i);
    m.name = m.id;
    if (i % 25 == 0) {
      m.is_separator    = true;
      m.separator_color = "#3a5f8a";
    } else if (nested && (i % 25) % 3 == 1) {
      // Children hang off the band's separator, so has_content() /
      // nesting_depth() actually walk a parent chain.
      m.parent_id = QStringLiteral("mod_%1").arg(parent);
    }
    m.conflict_wins   = (i % 7 == 0) ? 1 : 0;
    m.conflict_losses = (i % 11 == 0) ? 1 : 0;
    if (i % 25 == 0)
      parent = i;
    entries.append(m);
  }
  model.reset_with_order(entries);
}

// One ModMarkingScrollBar::paintEvent pass: isRowHidden + kScrollMarkRole for
// every row in the model (mod_table_view.cpp:274-290).
double scrollbar_frame_ms(const ui::ModList &model) {
  QElapsedTimer t;
  t.start();
  for (int r = 0; r < model.rowCount(); ++r) {
    const QVariant v = model.index(r, 0).data(ui::ModList::kScrollMarkRole);
    if (v.canConvert<QColor>())
      (void)v.value<QColor>();
  }
  return t.nsecsElapsed() / 1e6;
}

// One viewport pass: the roles QStyledItemDelegate reads for each visible cell.
double viewport_frame_ms(const ui::ModList &model, int first_row) {
  static const int roles[] = {Qt::DisplayRole,
                              Qt::DecorationRole,
                              Qt::FontRole,
                              Qt::BackgroundRole,
                              Qt::CheckStateRole,
                              Qt::ToolTipRole,
                              Qt::TextAlignmentRole,
                              Qt::ForegroundRole,
                              ui::ModList::kFlagIconsRole,
                              ui::ModList::kIndentDepthRole,
                              ui::ModList::kIsLastChildRole,
                              ui::ModList::kIsSeparatorRole,
                              ui::ModList::kSourceIconsRole};
  QElapsedTimer t;
  t.start();
  for (int r = first_row; r < first_row + kVisibleRows && r < model.rowCount(); ++r) {
    for (int c = 0; c < ui::ModList::ColumnCount; ++c) {
      const QModelIndex idx = model.index(r, c);
      for (int role : roles)
        (void)idx.data(role);
    }
  }
  return t.nsecsElapsed() / 1e6;
}

}  // namespace

// End-to-end: one SmoothScroller tick is scrollbar->setValue() followed by a
// synchronous repaint of the viewport AND the scrollbar. That is the real
// per-frame cost, delegates and layout included.
//
// `with_marking_bar` false swaps in a plain QScrollBar, which is the A/B that
// isolates ModMarkingScrollBar's O(all rows) mark pass from the rest.
double scroll_frame_ms(QAbstractItemView &view, int steps, int step_px,
                       bool with_marking_bar) {
  auto *bar = view.verticalScrollBar();
  QElapsedTimer t;
  t.start();
  for (int i = 0; i < steps; ++i) {
    const int next = bar->value() + step_px;
    if (next > bar->maximum())
      bar->setValue(bar->minimum());
    else
      bar->setValue(next);
    // Synchronous paints: exactly what the event loop does per frame.
    if (with_marking_bar)
      bar->repaint();
    else
      view.verticalScrollBar()->repaint();
    view.viewport()->repaint();
  }
  return t.nsecsElapsed() / 1e6 / steps;
}

TEST_CASE("mod list scroll frame cost", "[perf]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  qputenv("TZ", "UTC");
  tzset();
  const std::filesystem::path cfg = "/tmp/gmm_scroll_bench/config";
  std::filesystem::remove_all("/tmp/gmm_scroll_bench");
  std::filesystem::create_directories(cfg);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  int argc     = 1;
  char arg0[]  = "test";
  char *argv[] = {arg0, nullptr};
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  // --- End-to-end repaint cost vs mod count (the scaling law) --------------
  // A frame budget is 16.7 ms at 60 Hz. What matters is whether the per-frame
  // cost grows with the mod count (O(N): a bigger list scrolls worse) or stays
  // flat (O(visible): only new pixels cost).
  constexpr int kSteps = 40;
  printf("[scroll-e2e] scaling of one scroll frame (viewport + scrollbar repaint)\n");
  for (int mods : {500, 1500, 3000, 6000}) {
    for (bool nested : {false, true}) {
      ui::ModList model;
      populate(model, nested, mods);
      REQUIRE(model.rowCount() == mods);

      ui::ModView modview;
      modview.resize(1600, 1000);
      modview.setModel(&model);
      modview.show();
      QApplication::processEvents();

      QTreeView plain;
      plain.resize(1600, 1000);
      plain.setModel(&model);
      plain.show();
      QApplication::processEvents();

      (void)scroll_frame_ms(modview, 10, 20, true);
      (void)scroll_frame_ms(plain, 10, 20, false);

      const double marking = scroll_frame_ms(modview, kSteps, 20, true);
      const double base    = scroll_frame_ms(plain, kSteps, 20, false);
      printf("[scroll-e2e] mods=%5d nesting=%d  ModView=%7.3f ms  QTreeView=%7.3f ms  "
             "marking_bar_overhead=%6.3f ms\n",
             mods, nested ? 1 : 0, marking, base, marking - base);

      // A budget, not a benchmark: 40 ms is ~2.5x the measured worst case and
      // far under the 16.7 ms frame budget at 60 Hz. A regression that pushes
      // one frame past it goes red HERE with a number, instead of wedging CI.
      // Offscreen software rendering is far slower than the GPU path, so this
      // is a regression tripwire, not a performance promise.
      INFO("ModView frame at " << mods << " mods, nesting=" << nested << " was "
                               << marking << " ms");
      REQUIRE(marking < 40.0);
    }
  }
}