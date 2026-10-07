// The wheel must not switch the group or the profile dropdown.
//
// Both sit against the mod list, so a scroll meant for the list lands on them
// by accident: switching group silently hides rows, switching profile reloads a
// different mod list and load order. MO2 blocks exactly this with a one-line
// event filter on both combos (mainwindow.cpp:373-380) and so do we
// (ModFilterBar, ProfileBar).
//
// The filter only works if the wheel is DELIVERED to the combo, so that is
// pinned here too: Qt picks the receiver with QApplication::widgetAt(), and if
// a future Qt or a future widget change puts the popup container or its list
// view under the cursor, the filter is bypassed and this test goes red instead
// of the papercut coming back unnoticed.
//
// Delivered through QTest::wheelEvent -> QWindowSystemInterface, the same
// path a platform plugin uses, not a direct call into the filter. The wheel
// only moves a combo whose popup container already exists (QComboBox forwards
// the event into it), so every combo here is opened once before it is wheeled.
//
// Carries a liveness control: an unfiltered combo in the same process MUST
// move on the same wheel, so a dead dispatch cannot make this file pass
// vacuously.
//
// Hermetic: offscreen platform, no game, no instance, no files. Items are set
// with signals blocked so no profile switch or filter re-apply runs.
#include "ui/main_window/main_window.h"
#include "ui/widgets/mod_filter_bar.h"
#include "ui/widgets/profile_bar.h"

#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QScrollBar>
#include <QTest>
#include <qtestwheel.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

namespace {

// A dropdown mid-list, with its popup container built - the state in which a
// wheel would move the selection if nothing stopped it.
void arm(QComboBox *combo) {
  combo->blockSignals(true);
  combo->clear();
  combo->addItems({"all", "enabled", "disabled", "conflicts", "FOMOD", "separators",
                   "overwrite", "merged"});
  combo->setCurrentIndex(4);
  combo->blockSignals(false);
  combo->view();
  combo->showPopup();
  QCoreApplication::processEvents();
  combo->hidePopup();
  QCoreApplication::processEvents();
}

// One wheel notch over the widget's centre, the way a platform delivers it.
void wheel(QWindow *window, QComboBox *combo) {
  const QPoint centre = combo->mapToGlobal(combo->rect().center());
  QTest::wheelEvent(window, QPointF(centre), QPoint(0, -120), QPoint(0, 0),
                    Qt::NoModifier, Qt::NoScrollPhase);
}

}  // namespace

TEST_CASE("combo wheel: the wheel never switches group or profile", "[ui][filter]") {
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

  ui::MainWindow w;
  w.show();
  QCoreApplication::processEvents();

  auto *profile_combo = w.findChild<ui::ProfileBar *>()->findChild<QComboBox *>();
  auto *group_combo   = w.findChild<ui::ModFilterBar *>()->findChild<QComboBox *>();
  REQUIRE(profile_combo != nullptr);
  REQUIRE(group_combo != nullptr);
  auto *window_handle = w.windowHandle();
  REQUIRE(window_handle != nullptr);

  // Liveness control first: same wheel, same popup state, no filter. If this
  // one stops moving, the assertions below prove nothing.
  SECTION("an unfiltered combo does move, so the wheel below is live") {
    QWidget control_window;
    auto *control = new QComboBox(&control_window);
    control_window.resize(220, 60);
    control->setGeometry(10, 10, 200, 30);
    control_window.show();
    QCoreApplication::processEvents();
    arm(control);
    const int before = control->currentIndex();
    wheel(control_window.windowHandle(), control);
    CHECK(control->currentIndex() != before);
  }

  SECTION("the profile dropdown keeps its selection") {
    arm(profile_combo);
    const QPoint centre = profile_combo->rect().center();
    // The receiver really is the combo: this is what the event filter needs.
    CHECK(QApplication::widgetAt(profile_combo->mapToGlobal(centre)) == profile_combo);
    const int before = profile_combo->currentIndex();
    wheel(window_handle, profile_combo);
    CHECK(profile_combo->currentIndex() == before);
  }

  SECTION("the group dropdown keeps its selection") {
    arm(group_combo);
    const QPoint centre = group_combo->rect().center();
    CHECK(QApplication::widgetAt(group_combo->mapToGlobal(centre)) == group_combo);
    const int before = group_combo->currentIndex();
    wheel(window_handle, group_combo);
    CHECK(group_combo->currentIndex() == before);
  }

  SECTION("a focused dropdown keeps its selection too") {
    // Focus is not the lever: the filter runs before the combo's own handler
    // either way, and this is the case a user hits after clicking the combo.
    arm(group_combo);
    group_combo->setFocus();
    REQUIRE(group_combo->hasFocus());
    const int before = group_combo->currentIndex();
    wheel(window_handle, group_combo);
    CHECK(group_combo->currentIndex() == before);
  }

  SECTION("an open dropdown scrolls its list without switching") {
    // With the popup up the wheel lands on the popup's viewport, not on the
    // combo, so the filter cannot see it. Scrolling an open list is normal;
    // what must not happen is the selection changing underneath it.
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