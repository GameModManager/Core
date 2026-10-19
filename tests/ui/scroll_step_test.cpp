// One wheel notch must move the mod list as far as the panels and the console.
//
// Four dispests chased this as a frame-time problem and profiled paint costs.
// It is not a performance bug: it is the DISTANCE one notch travels, and it is
// decided by two things only - the view's verticalScrollMode(), and whatever
// consumes the wheel event before the view sees it. This file measures that
// distance on the real MainWindow, through a real notch, and pins the
// invariant the bug report describes: the mod list travels the same distance
// per notch as the panels beside it and the console under it.
//
// WHY THE MEASUREMENT IS DELICATE (each of these was a way to get a clean
// reading that meant nothing):
//
//  - The notch must be a real one. A wheel event carrying a pixelDelta takes
//    SmoothScroller's touchpad branch and never reaches the per-notch maths
//    that is under test. Delivered as angleDelta(0,-120) with a NULL pixelDelta,
//    which is what a mouse notch actually is.
//  - The receiver must be the viewport. QApplication::widgetAt() is how Qt
//    picks it, and a point off-screen is silently unhittable - so every arm
//    asserts the hit instead of trusting the geometry.
//  - The scroll step is animated over 180 ms, so reading the scrollbar straight
//    after the event reads 0 and every arm looks identical. Each arm settles
//    the event loop first.
//  - THE BIG ONE: the two families of widget count DIFFERENT UNITS. An item
//    view's vertical scrollbar is in PIXELS; a QPlainTextEdit's is in BLOCKS.
//    Reading both raw made the console report "3" next to the mod list's "48"
//    and look thirty times shorter when it is the same gesture. Everything
//    below is converted to pixels before anything is compared, so this file
//    cannot repeat the mistake it is guarding against.
//
// Hermetic: offscreen platform, throwaway XDG_CONFIG_HOME, no game, no files.
#include "ui/main_window/main_window.h"
#include "ui/widgets/console_panel.h"
#include "ui/widgets/mod_list_model.h"
#include "ui/widgets/mod_table_view.h"
#include "ui/widgets/right_panel.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFontMetricsF>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QSplitter>
#include <QThread>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTest>
#include <qtestwheel.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

namespace {

// The scroll animation runs for 180 ms; the scrollbar value is only final once
// the event loop has been pumped past that.
void settle() {
  QElapsedTimer t;
  t.start();
  while (t.elapsed() < 400)
    QCoreApplication::processEvents(QEventLoop::AllEvents, 16);
}

// Pixels one notch moves the given scroll area's vertical scrollbar by, through
// a real notch delivered the way a platform delivers it.
int notch_px(QAbstractScrollArea *area) {
  auto *window = area->window();
  auto *handle = window->windowHandle();
  REQUIRE(handle != nullptr);
  QScrollBar *bar = area->verticalScrollBar();
  REQUIRE(bar != nullptr);
  REQUIRE(bar->maximum() > 0);
  bar->setValue(bar->minimum());
  QCoreApplication::processEvents();

  const QPoint global =
      area->viewport()->mapToGlobal(area->viewport()->rect().center());
  // If the point is not on the viewport the wheel lands somewhere else and the
  // number below is fiction, so fail here rather than report a clean run.
  REQUIRE(QApplication::widgetAt(global) == area->viewport());
  const int before = bar->value();
  QTest::wheelEvent(handle, QPointF(window->mapFromGlobal(global)), QPoint(0, -120),
                    QPoint(0, 0), Qt::NoModifier, Qt::NoScrollPhase);
  settle();
  const int moved = bar->value() - before;
  REQUIRE(moved > 0);
  return moved;
}

void fill_mods(ui::ModList &model, int n) {
  QVector<ui::ModEntry> entries;
  entries.reserve(n);
  for (int i = 0; i < n; ++i) {
    ui::ModEntry m;
    m.id   = QStringLiteral("mod_%1").arg(i);
    m.name = QStringLiteral("Mod %1").arg(i);
    entries.append(m);
  }
  model.reset_with_order(entries);
}

}  // namespace

TEST_CASE("one wheel notch moves the mod list as far as the panels and the console",
          "[ui][scroll]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path root = "/tmp/opencode/gmm_scroll_step";
  std::filesystem::remove_all(root.c_str());
  std::filesystem::create_directories(root);
  qputenv("XDG_CONFIG_HOME", root.c_str());

  int argc     = 1;
  char a0[]    = "test";
  char *argv[] = {a0, nullptr};
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  // The offscreen screen is 800x800 and QApplication::widgetAt() refuses any
  // point no screen covers, so the window is sized to fit instead of the
  // natural 1200-wide layout.
  ui::MainWindow w;
  w.resize(760, 700);
  w.show();
  QCoreApplication::processEvents();

  auto *mod_view = w.findChild<ui::ModView *>();
  REQUIRE(mod_view != nullptr);
  REQUIRE(mod_view->verticalScrollMode() == QAbstractItemView::ScrollPerPixel);
  auto *model = qobject_cast<ui::ModList *>(mod_view->model());
  REQUIRE(model != nullptr);
  fill_mods(*model, 800);

  // Right panel: the Data tab's tree is the panel scrollable beside the mod
  // list. It is created inside RightPanel's constructor, i.e. before
  // enable_smooth_scrolling() runs, so it is swept by the same call - which is
  // exactly why it is the right comparator for the mod list.
  auto *panel = w.findChild<ui::RightPanel *>();
  REQUIRE(panel != nullptr);
  if (auto *tabs = panel->tab_widget())
    tabs->setCurrentIndex(0);
  auto *side = panel->findChild<QTreeWidget *>();
  REQUIRE(side != nullptr);
  side->clear();
  for (int r = 0; r < 500; ++r)
    side->addTopLevelItem(
        new QTreeWidgetItem(QStringList{QStringLiteral("side row %1").arg(r)}));

  // The console ships collapsed (console_splitter_->setSizes({700, 0})), and a
  // collapsed panel has no viewport to wheel - it would measure zero and read
  // as "scrolled nowhere" rather than "not measured".
  auto *console = w.findChild<QPlainTextEdit *>();
  REQUIRE(console != nullptr);
  if (auto *cp = w.findChild<ui::ConsolePanel *>()) {
    cp->show();
    if (auto *sp = qobject_cast<QSplitter *>(cp->parentWidget()))
      sp->setSizes({400, 280});
  }
  console->setLineWrapMode(QPlainTextEdit::NoWrap);
  for (int i = 0; i < 300; ++i)
    console->appendPlainText(QStringLiteral("console output line %1").arg(i));

  QCoreApplication::processEvents();
  QCoreApplication::processEvents();

  const int mod_px  = notch_px(mod_view);
  const int side_px = notch_px(side);

  // The console counts BLOCKS; convert with its own line height so the three
  // arms are compared in pixels and not in whatever unit each happens to use.
  const qreal line_px      = QFontMetricsF(console->font()).lineSpacing();
  const int console_blocks = notch_px(console);
  const int console_px     = qRound(console_blocks * line_px);

  INFO("mod list " << mod_px << " px/notch, side panel " << side_px
                   << " px/notch, console " << console_px << " px/notch ("
                   << console_blocks << " lines of " << line_px << "px)");

  // The invariant the bug report is about: the mod list must travel the same
  // distance per notch as the panel beside it. Exact equality, not a range -
  // both are item views swept by the same enable_smooth_scrolling() call, so
  // any difference between them is a real asymmetry, and asymmetry is the bug.
  CHECK(mod_px == side_px);

  // Parity alone would pass if EVERYTHING were shortened together, so the
  // console anchors the absolute distance: a notch is a few lines of text, and
  // the mod list must stay within one line of that. This is the assertion that
  // goes red when the per-notch maths itself is shortened (see the kLinesPerNotch
  // mutation in the negative control), which the parity check cannot see.
  CHECK(std::abs(mod_px - console_px) <= qRound(line_px));

  // The wiring, not the default. Every check above passes with the old
  // hardcoded 3 simply because 3 IS Qt's default - which is how a hardcoded
  // value can be wrong and still look right here. Move the setting the console
  // and every other scrollable read and the mod list must move with it.
  constexpr int kBefore = 3;
  constexpr int kAfter  = 9;
  const int base        = QApplication::wheelScrollLines();
  REQUIRE(base == kBefore);
  QApplication::setWheelScrollLines(kAfter);
  const int mod_px_wide = notch_px(mod_view);
  QApplication::setWheelScrollLines(base);
  INFO("mod list " << mod_px << " px/notch at " << kBefore << " lines, " << mod_px_wide
                   << " px/notch at " << kAfter << " lines");
  CHECK(mod_px_wide == mod_px * (kAfter / kBefore));

  std::filesystem::remove_all(root.c_str());
}
