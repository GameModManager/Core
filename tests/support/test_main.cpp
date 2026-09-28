// Shared Catch2 runner for every Core test target (Workspace-2zm).
//
// Why this file exists: production code builds QSettings with an explicit
// format - QSettings(QSettings::defaultFormat(), QSettings::UserScope,
// "GameModManager", "GameModManager") in src/ui/settings/settings.h and in
// plugin_settings_registry.cpp. In a real run nothing calls
// setDefaultFormat(), so defaultFormat() is NativeFormat and the store is
// byte-identical to what the old (organization, application) constructor used
// (same file, same key space, same fallback chain). In a test run THIS file
// calls setDefaultFormat(IniFormat), so the same production expression becomes
// a temp .ini and the user's real config is unreachable.
//
// NativeFormat writes to ~/Library/Preferences on macOS (CFPreferences plist),
// to the registry on Windows, and to $HOME/.config on Unix. None of those are
// redirected by format, so the fix has to come from the FORMAT, not from a
// path: IniFormat + setPath(IniFormat, UserScope, dir) is honoured on every
// platform (Qt docs, qsettings.html - only setPath(NativeFormat) is a
// documented no-op on Windows/macOS/iOS). No environment variable is
// involved in the isolation itself, so it cannot differ per platform.
//
// The isolation lives here, ONCE, so a new test inherits it and nobody can
// forget to copy the setup block:
//   1. setDefaultFormat(IniFormat) - the decisive step. The three production
//      sites read defaultFormat() when they construct their QSettings, so
//      every one of them - present and future - lands in the sandbox. The
//      default QSettings() ctor and explicit IniFormat users come along for
//      free.
//   2. setPath(IniFormat, UserScope, dir) - where that ini actually goes.
//   3. point XDG_CONFIG_HOME at the same dir FIRST, before any QSettings API
//      call. This is belt-and-braces, NOT the mechanism: the native .conf path
//      is resolved once, from the environment the process sees at the first
//      QSettings call, so this covers anything still built with a native ctor
//      on Unix, plus the direct file consumers (std::filesystem, QFile) that
//      follow XDG_CONFIG_HOME. Setting it later is silently ignored. It is
//      Unix-only by nature - Windows has no equivalent variable - and nothing
//      depends on it: the isolation that matters is step 1 + step 2, which are
//      platform-independent. No test in the suite reads the config path
//      directly, so if Windows CI ever appears there is no gap to close here.
//   4. remove the temp dir at exit.
//
// Note on per-test XDG overrides: a test may still call
// qputenv("XDG_CONFIG_HOME", itsOwnDir). That keeps working for direct file
// consumers, but QSettings itself keeps using the process-wide sandbox from
// step 1 - reads and writes still go to the SAME place, so round-trips are
// unaffected; only the on-disk location differs from what the test's env var
// says. Nothing in the suite asserts on that.
//
// Per-test/pere-process uniqueness: CTest runs one TEST_CASE per process
// (catch_discover_tests registers each case as its own test), so every case
// gets a fresh sandbox. Cases sharing a single process (running the binary
// by hand) share one sandbox - same as before this fixture, when they all
// shared the first XDG_CONFIG_HOME the process saw.
//
// Escape hatch - the unsandboxed process: GMM_TEST_NATIVE_QSETTINGS=1 skips
// everything above, so the process sees exactly what a real run sees. Used by
// qsettings_native_format_test, which exercises the real Settings and
// PluginSettingsRegistry against a native store and therefore WRITES. It owns
// its isolation completely: CTest launches it with a throwaway HOME (see
// tests/CMakeLists.txt) and the test itself refuses to write unless the
// resolved store is inside that home. Any other test that opts out must do
// the same.

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

  // Order: the env var first, because the native .conf path is resolved from
  // the environment at the first QSettings API call. The three steps are
  // independent of each other after that; the format flip is what redirects
  // the production sites, the path table says where the ini goes.
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
