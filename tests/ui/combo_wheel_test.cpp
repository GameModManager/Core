// The wheel must not switch a CLOSED dropdown - anywhere in the app.
//
// A scroll meant for the widget next to a combo lands on the combo by
// accident: switching group silently hides rows, switching profile reloads a
// different mod list and load order, and the program dropdown beside the Run
// button opens the Executables editor because every entry it holds has no item
// data, which its own currentIndexChanged reads as "sentinel selected".
// MO2 stops the same accident with a one-line event filter on the two combos
// beside the mod list (mainwindow.cpp:373-380). A filter installed on a combo
// only ever sees that one combo, though, so doing it there left the other
// twenty dropdowns scrolling. install_combo_wheel_guard() puts the same one
// line on the QApplication object, where Qt runs it ahead of the receiver's
// own handler, and it covers combos that do not exist yet.
//
// So this file exercises a combo nobody wired by hand (the program dropdown in
// ExecControlsBar) and a combo that is not in any app widget tree at all, which
// a per-widget install could not reach either way.
//
// The guard only works if the wheel is DELIVERED to the combo, so that is
// pinned too: Qt picks the receiver with QApplication::widgetAt(), and if a
// future Qt or a future widget change puts the popup container or its list view
// under the cursor, the guard is bypassed and this test goes red instead of the
// papercut coming back unnoticed. Note that widgetAt is gated on the point
// landing on a screen, which is why the window is resized below.
//
// Delivered through QTest::wheelEvent -> QWindowSystemInterface, the same path
// a platform plugin uses, not a direct call into the guard. The wheel only
// moves a combo whose popup container already exists (QComboBox forwards the
// event into it), so every combo here is opened once before it is wheeled.
//
// The liveness control is the guard taken back out: the same combo, on the same
// wheel, in the same popup state, MUST move once the guard is off the
// application. Nothing below can pass on a dead dispatch, and nothing below can
// pass if the guard stops filtering.
//
// Hermetic: offscreen platform, no game, no instance, no files. Items are set
// with signals blocked so no profile switch or filter re-apply runs.
#include "ui/main_window/main_window.h"
#include "ui/widgets/event_filter.h"
#include "ui/widgets/exec_controls_bar.h"
#include "ui/widgets/mod_filter_bar.h"
#include "ui/widgets/profile_bar.h"

#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QJsonObject>
#include <QScrollBar>
#include <QStringList>
#include <QTest>
#include <QVariant>
#include <qtestwheel.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

namespace {

// A dropdown mid-list, with its popup container built - the state in which a
// wheel would move the selection if nothing stopped it.
//
// The entries carry a "path" in their item data on purpose. ExecControlsBar
// reads itemData as the executable's JSON and treats an entry with none as the
// "add a new one" sentinel, so moving that dropdown's index opens the
// Executables editor over the test. A populated list is also what the combo
// really looks like, and it means a regression fails this file instead of
// hanging it.
void arm(QComboBox *combo) {
  static const QStringList names = {"all",   "enabled",    "disabled",  "conflicts",
                                    "FOMOD", "separators", "overwrite", "merged"};
  combo->blockSignals(true);
  combo->clear();
  for (const QString &name : names)
    combo->addItem(name,
                   QVariant(QJsonObject{{QStringLiteral("path"),
                                         QStringLiteral("/opt/games/bin/") + name}}));
  combo->setCurrentIndex(4);
  combo->blockSignals(false);
  combo->view();
  combo->showPopup();
  QCoreApplication::processEvents();
  combo->hidePopup();
  QCoreApplication::processEvents();
}

// One wheel notch over the widget's centre, the way a platform delivers it.
// QTest::wheelEvent takes the position in the target window's own coordinates
// and maps it to global itself, so it has to be mapped to the window here.
void wheel(QWindow *window, QComboBox *combo) {
  const QPoint centre =
      combo->window()->mapFromGlobal(combo->mapToGlobal(combo->rect().center()));
  QTest::wheelEvent(window, QPointF(centre), QPoint(0, -120), QPoint(0, 0),
                    Qt::NoModifier, Qt::NoScrollPhase);
}

}  // namespace

TEST_CASE("combo wheel: the wheel never switches a closed dropdown", "[ui][filter]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path root = "/tmp/opencode/gmm_combo_wheel/config";
  std::filesystem::remove_all("/tmp/opencode/gmm_combo_wheel");
  std::filesystem::create_directories(root);
  qputenv("XDG_CONFIG_HOME", root.c_str());

  int argc     = 1;
  char a0[]    = "test";
  char *argv[] = {a0, nullptr};
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  // A dropdown in a window of its own, with nothing in the app attached to it.
  // Not in the app's widget tree at all and never handed a filter by hand, so
  // only an application-wide guard can be the reason it holds still. It is also
  // the negative control's combo: it has no slots wired to its index, so the one
  // wheel that moves it has nowhere else to go.
  //
  // Shown BEFORE the main window and parked in the strip the main window does
  // not cover, because offscreen makes the last shown window the active one
  // (only the active window's widgets can take focus) and because a window
  // sitting under the main window is not the widget the wheel reaches.
  QWidget bare_window;
  auto *bare_combo = new QComboBox(&bare_window);
  bare_window.resize(300, 70);
  bare_combo->setGeometry(10, 10, 280, 30);
  bare_window.show();
  QCoreApplication::processEvents();
  auto *bare_handle = bare_window.windowHandle();
  REQUIRE(bare_handle != nullptr);

  ui::MainWindow w;
  // The offscreen screen is 800x800 and QApplication::widgetAt() (which is how
  // Qt picks the wheel's receiver) refuses any point that no screen covers, so
  // the natural 1200-wide window puts the right-hand dropdown at x=886, off
  // screen and unhittable. Shrink to fit every combo on screen; the window's
  // own minimum size is 615x396. The bottom strip is left free for
  // bare_window. If a layout change later pushes a combo off screen or under
  // another window, the widgetAt REQUIRE below says so instead of the wheel
  // silently landing on nothing.
  w.resize(760, 700);
  w.show();
  bare_window.move(2, 710);
  QCoreApplication::processEvents();

  // The one install the app makes too, in the same place: on the application.
  auto *guard = ui::install_combo_wheel_guard(&app);

  auto *profile_combo = w.findChild<ui::ProfileBar *>()->findChild<QComboBox *>();
  auto *group_combo   = w.findChild<ui::ModFilterBar *>()->findChild<QComboBox *>();
  auto *exec_combo    = w.findChild<ui::ExecControlsBar *>()->findChild<QComboBox *>();
  REQUIRE(profile_combo != nullptr);
  REQUIRE(group_combo != nullptr);
  REQUIRE(exec_combo != nullptr);
  auto *window_handle = w.windowHandle();
  REQUIRE(window_handle != nullptr);

  // Wheel the combo the way a platform delivers it and report whether the
  // selection moved. The receiver is asserted first: if the combo is not what
  // is under the cursor the wheel says nothing, so a hidden or covered combo
  // fails here instead of passing vacuously below.
  auto wheel_moves = [&](QComboBox *combo, QWindow *window) {
    REQUIRE(window != nullptr);
    const QPoint centre = combo->rect().center();
    REQUIRE(QApplication::widgetAt(combo->mapToGlobal(centre)) == combo);
    const int before = combo->currentIndex();
    wheel(window, combo);
    return combo->currentIndex() != before;
  };

  SECTION("with the guard off the application, the wheel does move a combo") {
    // Liveness, and the negative control in one: same combo, same wheel, same
    // popup state, guard removed. If this stops moving, the checks below are
    // measuring nothing.
    arm(bare_combo);
    qApp->removeEventFilter(guard);
    const bool moved = wheel_moves(bare_combo, bare_handle);
    qApp->installEventFilter(guard);
    CHECK(moved);
  }

  SECTION("the program dropdown keeps its selection") {
    // Never had a filter installed on it by hand. It is the combo beside the
    // Run button, so a stray scroll there changes what the user launches - and
    // because every entry it holds was added without any item data, moving its
    // index also fires add_entry_requested, which opens the Executables editor
    // over the app. Nothing here reaches that: the wheel is stopped before the
    // combo sees it, which is the whole point.
    arm(exec_combo);
    CHECK_FALSE(wheel_moves(exec_combo, window_handle));
  }

  SECTION("a dropdown outside every app widget tree keeps its selection too") {
    // Same combo as the negative control, guard back on: the only difference
    // between the two sections is the guard, so the pair pins it as the reason.
    arm(bare_combo);
    CHECK_FALSE(wheel_moves(bare_combo, bare_handle));
  }

  SECTION("the profile dropdown keeps its selection") {
    arm(profile_combo);
    CHECK_FALSE(wheel_moves(profile_combo, window_handle));
  }

  SECTION("the group dropdown keeps its selection") {
    arm(group_combo);
    CHECK_FALSE(wheel_moves(group_combo, window_handle));
  }

  SECTION("a focused dropdown keeps its selection too") {
    // Focus is not the lever: the guard runs before the combo's own handler
    // either way, and this is the case a user hits after clicking the combo.
    arm(group_combo);
    group_combo->setFocus();
    REQUIRE(group_combo->hasFocus());
    CHECK_FALSE(wheel_moves(group_combo, window_handle));
  }

  SECTION("an open dropdown scrolls its list without switching") {
    // With the popup up the wheel lands on the popup's viewport, which is not
    // the combo, so the guard lets it through on purpose. Scrolling an open
    // list is normal; what must not happen is the selection changing
    // underneath it. Deliberately the profile dropdown, not the program one:
    // if a future Qt did move the selection, the program dropdown would open
    // the Executables editor and hang the test rather than report it.
    arm(profile_combo);
    profile_combo->showPopup();
    QCoreApplication::processEvents();
    const int before = profile_combo->currentIndex();
    wheel(window_handle, profile_combo);
    CHECK(profile_combo->currentIndex() == before);
    profile_combo->hidePopup();
  }

  std::filesystem::remove_all("/tmp/opencode/gmm_combo_wheel");
}