// Shared Catch2 runner for every Core test target (Workspace-2zm).
//
// Why this file exists: production code builds QSettings with the
// (organization, application) constructor
// (QSettings settings_{"GameModManager", "GameModManager"} in
// src/ui/settings/settings.h), and that constructor ALWAYS uses
// NativeFormat - QSettings::setDefaultFormat() has no effect on it (Qt docs,
// qsettings.html). NativeFormat writes to ~/Library/Preferences on macOS
// (CFPreferences plist) and to $HOME/.config on Unix, so before this fixture
// every test that reached Settings::instance() - directly or through a
// widget - read and wrote the user's REAL settings. That is a live
// correctness bug (tests can permanently change the user's configuration)
// and a source of cross-run "unexplained" settings-test flakiness.
//
// The isolation lives here, ONCE, so a new test inherits it and nobody can
// forget to copy the setup block:
//   1. point XDG_CONFIG_HOME at a process-unique temp dir FIRST - before any
//      QSettings API call. The native .conf path is resolved once, from the
//      environment the process sees at that first call; setting it later is
//      silently ignored (verified with probe4: a later qputenv does not move
//      an already-resolved QSettings path),
//   2. setPath(NativeFormat/IniFormat, UserScope, dir) - on Unix both formats
//      share the same UserScope path table, so this makes the redirect
//      deterministic regardless of who calls QSettings first. (On
//      Windows/macOS/iOS setPath(NativeFormat) is a documented no-op - the
//      env redirect above is what covers Unix, and any test that must touch
//      platform-native backends uses the opt-in below),
//   3. force QSettings IniFormat + setPath(IniFormat) as well, so the default
//      QSettings() constructor and explicit IniFormat users land in the same
//      sandbox,
//   4. remove the temp dir at exit.
//
// Note on per-test XDG overrides: a test may still call
// qputenv("XDG_CONFIG_HOME", itsOwnDir). That keeps working for direct file
// consumers (std::filesystem, QFile), but QSettings itself keeps using the
// process-wide sandbox from step 1 - reads and writes still go to the SAME
// place, so round-trips are unaffected; only the on-disk location differs
// from what the test's env var says. Nothing in the suite asserts on that.
//
// Per-test/pere-process uniqueness: CTest runs one TEST_CASE per process
// (catch_discover_tests registers each case as its own test), so every case
// gets a fresh sandbox. Cases sharing a single process (running the binary
// by hand) share one sandbox - same as before this fixture, when they all
// shared the first XDG_CONFIG_HOME the process saw.
//
// Escape hatch - explicit NativeFormat opt-in: a test that genuinely must
// exercise NativeFormat runs with GMM_TEST_NATIVE_QSETTINGS=1 in its CTest
// ENVIRONMENT properties (or on the command line) and is then responsible
// for its own config isolation. There is no such test today: nothing in the
// suite asserts on platform-native settings backends.

#include <QDir>
#include <QSettings>
#include <QUuid>

#include <catch2/catch_session.hpp>

#include <cstdio>
#include <cstdlib>

namespace {

// Process-unique sandbox root. A namespace-scope object: constructed before
// main(), so it outlives the atexit handler below (which is registered later
// and therefore runs first).
QString sandbox_dir;

void remove_sandbox() {
  if (!sandbox_dir.isEmpty())
    QDir(sandbox_dir).removeRecursively();
}

void sandbox_settings() {
  // Explicit opt-in: the test wants the real platform backend and owns its
  // own isolation (see the header comment).
  if (qEnvironmentVariableIsSet("GMM_TEST_NATIVE_QSETTINGS"))
    return;

  sandbox_dir = QDir::tempPath() + "/gmm_qsettings_" +
                QUuid::createUuid().toString(QUuid::WithoutBraces);
  QDir().mkpath(sandbox_dir);
  // Registered before any Settings/QSettings static exists (those are
  // constructed during the run), so their destructors sync first and this
  // removes the dir afterwards. A crashed test can still leave one behind in
  // the system temp dir - never in the user's config.
  std::atexit(remove_sandbox);

  // Order matters: env first (the native path is resolved from the
  // environment at the first QSettings API call), then the explicit path
  // table overrides.
  qputenv("XDG_CONFIG_HOME", sandbox_dir.toLocal8Bit());
  QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope,
                     sandbox_dir);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, sandbox_dir);
  QSettings::setDefaultFormat(QSettings::IniFormat);
}

}  // namespace

int main(int argc, char *argv[]) {
  // Must happen before QApplication and before the first QSettings: the path
  // is fixed when the first QSettings API call runs.
  sandbox_settings();
  return Catch::Session().run(argc, argv);
}
