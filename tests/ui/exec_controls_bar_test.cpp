// Offscreen GUI regression test for ExecControlsBar's "<Edit...>" sentinel
// handling (Workspace-vwa):
//   - With zero executables the sentinel is the only item and already current;
//     picking it must still request the entry editor. Qt emits activated()
//     even when the choice does not change, while currentIndexChanged() only
//     fires on an actual index change - so the 0-executables fallback is
//     wired to activated().
//   - Picking the sentinel with entries present requests the editor AND keeps
//     the previous real selection (restore logic). This is wired to
//     currentIndexChanged() because Qt's hidePopup() emits
//     activated(currentIndex()) after setCurrentIndex(), meaning the
//     currentIndexChanged handler's restore shifts currentIndex before
//     activated fires — making activated()'s index parameter unreliable for
//     detecting the sentinel in the entries-present case.
//   - Programmatic rebuilds (set_executables/clear_executables) never request
//     the editor.
//
// Hermetic: no files, no network. QT_QPA_PLATFORM=offscreen via the test
// property.
#include "ui/widgets/exec_controls_bar.h"

#include <QApplication>
#include <QComboBox>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("exec controls bar edit sentinel", "[ui]") {
  int test_argc = 1;
  char test_argv0[] = "test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);

  ui::ExecControlsBar bar;
  int add_requests = 0;
  QObject::connect(&bar, &ui::ExecControlsBar::add_entry_requested,
                   [&] { ++add_requests; });
  auto *combo = bar.findChild<QComboBox *>();
  REQUIRE(combo != nullptr);

  SECTION("empty list: picking the already-current sentinel opens the editor") {
    REQUIRE(combo->count() == 1);
    REQUIRE(combo->currentIndex() == 0);
    // User picks <Edit...> in the popup: no index change, so Qt emits
    // only activated().
    emit combo->activated(0);
    CHECK(add_requests == 1);
  }

  SECTION("programmatic rebuilds never open the editor") {
    bar.set_executables({"game.exe"});
    CHECK(add_requests == 0);
    bar.clear_executables();
    CHECK(add_requests == 0);
    CHECK(combo->count() == 1); // bare sentinel re-added
  }

  SECTION(
      "entries present: sentinel click opens editor and restores selection") {
    bar.set_executables({"a.exe", "b.exe"}, "b.exe");
    REQUIRE(combo->count() == 3);
    REQUIRE(combo->currentIndex() == 2);
    add_requests = 0;
    // Real Qt order when the user picks the sentinel from the popup:
    // currentIndexChanged(0) first (restore + emit), then
    // activated() with the post-restore currentIndex (not 0).
    // add_entry_requested fires from currentIndexChanged, not activated.
    combo->setCurrentIndex(0);
    // activated fires after currentIndexChanged with the restored index;
    // the activated handler guards against double-fire via
    // current_executable().isEmpty().
    emit combo->activated(2); // Qt emits activated(currentIndex()) post-restore
    CHECK(add_requests == 1);
    CHECK(combo->currentIndex() == 2); // restored, not left on sentinel
  }
}
