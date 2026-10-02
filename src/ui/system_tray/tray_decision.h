#pragma once

// ---------------------------------------------------------------------------
// Tray decisions
// ---------------------------------------------------------------------------
// Whether closing the main window quits the app or hides it to the tray, and
// what a "bring the window back" request does. Split out from the QSystemTrayIcon
// calls for the same reason install-method detection is: the branches are the
// whole feature, and they have to be reachable in a test on a machine with no
// tray at all. Nothing here includes Qt or touches a window.
//
// The split follows detect_install_method(InstallFacts): a facts struct fed by
// probes, and a pure function over it. Here every fact is a bool the caller
// already had to know anyway - whether a tray exists, what the user asked for,
// whether the app is already on its way out.
//
// Two rules outrank every other consideration here, because getting them wrong
// loses the user's app rather than merely annoying them:
//
//   1. No tray means no hiding. If the system tray cannot be initialised the
//      window has to keep its ordinary meaning, or the only way back is a
//      tray icon that does not exist.
//   2. Hiding is opt-in. The setting defaults off, so the default close
//      quits - the same as pressing the X on any other application.

namespace ui::tray {

// What the user (or the OS, on second launch) is asking for.
enum class TrayIntent {
  // The window was closed: titlebar X, Alt+F4, or close() from elsewhere.
  CloseWindow,
  // The window should be brought back: tray click, tray menu, or another
  // instance asking us to come forward.
  ShowWindow,
};

enum class TrayAction {
  // Keep running with no window. The tray icon is the app's only face now.
  HideToTray,
  // Unhide, raise and focus. Always safe, whatever the tray is doing.
  RestoreWindow,
  // Close for real. Every teardown step in closeEvent still runs.
  QuitWindow,
};

// Everything the decision is made from.
struct TrayFacts {
  // QSystemTrayIcon::isSystemTrayAvailable(), probed once at startup. A tray
  // that appears later does not count: the window may already have been told
  // it was safe to hide.
  bool tray_available = false;

  // The user's minimize-to-tray preference. Default false.
  bool minimize_to_tray = false;

  // The app is already shutting down (tray Quit, qApp quit, signal). Hiding
  // here would leave a running process with no way to reach it.
  bool quitting = false;
};

// The decision.
//
// RestoreWindow is deliberately not gated on tray_available: the request can
// arrive from another instance over IPC, long after the tray was probed, and
// dropping it would leave the launcher's second instance doing nothing at all.
inline TrayAction decide_tray_action(TrayFacts facts, TrayIntent intent) {
  if (intent == TrayIntent::ShowWindow)
    return TrayAction::RestoreWindow;
  if (facts.quitting || !facts.tray_available || !facts.minimize_to_tray)
    return TrayAction::QuitWindow;
  return TrayAction::HideToTray;
}

}  // namespace ui::tray
