// Offscreen GUI test for ConsolePanel - the in-window log view fed by
// engine::Logger. Covers the on-screen retention window: a long session must
// not grow the document without bound, and the lines that survive must be the
// NEWEST ones (the tail is what a user reads when something goes wrong).
// The text view is reached through findChild, so the assertions read the real
// rendered document rather than a mirrored counter. No filesystem, no engine
// logging, no network; lines are pushed through the same public append path
// the Logger callback uses. QT_QPA_PLATFORM=offscreen comes from the test
// property.
#include "ui/widgets/console_panel.h"

#include <QApplication>
#include <QPlainTextEdit>

#include <catch2/catch_test_macros.hpp>

namespace {

// The shared ctest helper drops QT_QPA_PLATFORM, so force offscreen here.
QApplication &test_app() {
  static int argc     = 1;
  static char argv0[] = "test";
  static char *argv[] = {argv0, nullptr};
  qputenv("QT_QPA_PLATFORM", "offscreen");
  static QApplication app(argc, argv);
  return app;
}

void check(bool cond, const char *what) {
  INFO(what);
  REQUIRE(cond);
}

// The panel's text view. Returns the real widget so toPlainText() reports what
// is on screen.
QPlainTextEdit *view_of(ui::ConsolePanel &panel) {
  auto *out = panel.findChild<QPlainTextEdit *>();
  REQUIRE(out != nullptr);
  return out;
}

// Lines on screen, counted the way a user reads them: one per newline. Qt's
// block count carries a trailing empty block that is not a log line, so
// counting newlines is the honest measure and does not depend on Qt's block
// bookkeeping.
int lines_on_screen(ui::ConsolePanel &panel) {
  return static_cast<int>(view_of(panel)->toPlainText().count('\n'));
}

// One log line through the real render path (tag + timestamp + message, each
// with its own character format).
void push(ui::ConsolePanel &panel, const QString &message) {
  panel.append_log("INF", "12:00:00", message, 1 /* LogLevel::Info */);
}

}  // namespace

TEST_CASE("ConsolePanel bounds the on-screen log", "[ui][console]") {
  test_app();
  auto *panel = new ui::ConsolePanel();

  SECTION("starts empty") {
    check(lines_on_screen(*panel) == 0, "a fresh panel has no lines");
  }

  SECTION("keeps every line while under the cap") {
    for (int i = 0; i < 10; ++i)
      push(*panel, QStringLiteral("line %1").arg(i));
    check(lines_on_screen(*panel) == 10, "10 lines are all retained");
  }

  SECTION("caps at kMaxLines and keeps the newest lines") {
    // Feed well past the window. Each line is unique, so which lines survive
    // identifies exactly which end of the log was kept.
    const int kFed = ui::ConsolePanel::kMaxLines + 250;
    for (int i = 0; i < kFed; ++i)
      push(*panel, QStringLiteral("line %1").arg(i));

    INFO("lines on screen: " << lines_on_screen(*panel) << " of " << kFed << " fed");
    // Qt holds one trailing empty block inside maximumBlockCount, so a capped
    // panel shows one line fewer than the cap. Bound the window rather than
    // pinning Qt's bookkeeping: what matters is that it stops growing, is
    // filled, and keeps the tail.
    check(lines_on_screen(*panel) <= ui::ConsolePanel::kMaxLines,
          "the view is capped, not merely grown slowly");
    check(lines_on_screen(*panel) >= ui::ConsolePanel::kMaxLines - 10,
          "the window is actually full, not collapsed to nothing");

    const QString text = view_of(*panel)->toPlainText();
    check(!text.contains(QStringLiteral("line 0\n")),
          "the oldest line is the one dropped");
    check(text.contains(QStringLiteral("line %1\n").arg(kFed - 1)),
          "the newest line is retained");
  }

  SECTION("clear empties the capped document") {
    for (int i = 0; i < 50; ++i)
      push(*panel, QStringLiteral("line %1").arg(i));
    panel->clear();
    check(lines_on_screen(*panel) == 0, "clear() resets the view");
  }

  delete panel;
}
