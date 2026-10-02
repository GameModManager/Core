// Tray close/restore decisions.
//
// The branches here are the whole feature, and two of them lose the user's
// application rather than merely annoy them, so they are pinned rather than
// left to a GUI check:
//
//   - close quits by default, so the titlebar X means what it means everywhere
//     else unless the user opted into the tray;
//   - no tray means no hiding, whatever the setting says, because a window
//     hidden behind an icon that cannot exist is unreachable;
//   - quitting never hides, or the tray's own Quit would leave a running
//     process with no way back;
//   - a "show me" request is never dropped, because it arrives from a second
//     launch over IPC where the tray's state is irrelevant.
//
// Qt-free and platform-free, so this runs anywhere. Nothing here needs a
// QApplication, a tray, or a window.
#include "ui/system_tray/tray_decision.h"

#include <catch2/catch_test_macros.hpp>

using ui::tray::decide_tray_action;
using ui::tray::TrayAction;
using ui::tray::TrayFacts;
using ui::tray::TrayIntent;

TEST_CASE("closing the window quits unless the user asked for the tray", "[ui][tray]") {
  TrayFacts facts;
  facts.tray_available   = true;
  facts.minimize_to_tray = false;

  CHECK(decide_tray_action(facts, TrayIntent::CloseWindow) == TrayAction::QuitWindow);

  // The opt-in. Both preconditions hold, so this is the one path that hides.
  facts.minimize_to_tray = true;
  CHECK(decide_tray_action(facts, TrayIntent::CloseWindow) == TrayAction::HideToTray);
}

TEST_CASE("a desktop with no tray never hides the window", "[ui][tray]") {
  TrayFacts facts;
  facts.tray_available   = false;
  facts.minimize_to_tray = true;  // the setting alone must not strand the app

  CHECK(decide_tray_action(facts, TrayIntent::CloseWindow) == TrayAction::QuitWindow);
}

TEST_CASE("quitting is never turned into a hide", "[ui][tray]") {
  TrayFacts facts;
  facts.tray_available   = true;
  facts.minimize_to_tray = true;
  facts.quitting         = true;

  CHECK(decide_tray_action(facts, TrayIntent::CloseWindow) == TrayAction::QuitWindow);
}

TEST_CASE("a show request is honoured whatever the tray is doing", "[ui][tray]") {
  // Second launch over IPC, tray probed long ago and now gone: dropping this
  // would make the launcher relaunch look like it did nothing at all.
  TrayFacts no_tray;
  no_tray.tray_available   = false;
  no_tray.minimize_to_tray = true;
  no_tray.quitting         = true;

  CHECK(decide_tray_action(no_tray, TrayIntent::ShowWindow) ==
        TrayAction::RestoreWindow);

  TrayFacts quitting;
  quitting.tray_available   = true;
  quitting.minimize_to_tray = true;
  quitting.quitting         = true;

  CHECK(decide_tray_action(quitting, TrayIntent::ShowWindow) ==
        TrayAction::RestoreWindow);
}