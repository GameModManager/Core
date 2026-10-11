// lock_gui (MO2 InterfaceSettings::lockGUI, references/modorganizer/src/
// settings.cpp:2168) gates ui::Locker::lock().
//
// The gate that matters: lock_gui=false suppresses the plain LockUI lock, but
// NOT an OutputRequired one. MO2 overrides the setting for OutputRequired
// (processrunner.cpp:860-868) because the install's own output is what
// finishes it and two concurrent installs are impossible, so there is nothing
// to race. A test that only checked the suppression would stay green with that
// override deleted - the case below checks both in one run.
//
// Also covered: unlock() is unconditional (the gate is not symmetric, or a
// setting change mid-operation would strand a greyed-out window), and a widget
// that was already disabled before the lock is not re-enabled by it (MO2
// uilocker.cpp:630 - disable() only touches widgets that are currently
// enabled, so unlock() cannot undo a deliberate "off" from elsewhere).
//
// Hermetic: offscreen platform, per-case unique /tmp scratch root (pid +
// sequence) with a throwaway XDG_CONFIG_HOME/XDG_DATA_HOME, so the lock_gui
// value never lands in the real GameModManager.conf. No network, no plugins.
#include "ui/main_window/main_window.h"
#include "ui/settings/settings.h"
#include "ui/ui_locker.h"

#include <QApplication>
#include <QByteArray>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

// Per-TEST_CASE unique scratch root: ctest runs each TEST_CASE as its own
// concurrent worker process, so the pid disambiguates parallel processes and
// the atomic sequence disambiguates cases within one process.
struct ScopedScratch {
  std::filesystem::path root;
  std::filesystem::path config;  // throwaway XDG_CONFIG_HOME
  std::filesystem::path data;    // throwaway XDG_DATA_HOME

  ScopedScratch() {
    static std::atomic<unsigned> seq{0};
#if defined(_WIN32)
    const auto pid = static_cast<unsigned long>(_getpid());
#else
    const auto pid = static_cast<unsigned long>(::getpid());
#endif
    root = std::filesystem::temp_directory_path() /
           ("gmm_lock_gui_" + std::to_string(pid) + "_" +
            std::to_string(seq.fetch_add(1, std::memory_order_relaxed)));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);  // stale dir from a crashed run
    config = root / "config";
    data   = root / "data";
    std::filesystem::create_directories(config, ec);
    std::filesystem::create_directories(data, ec);
  }
  ~ScopedScratch() noexcept {
    std::error_code ec;  // never throw from the dtor
    std::filesystem::remove_all(root, ec);
  }
};

// Env goes before the QApplication emplace (it is read at construction).
struct CaseSetup {
  ScopedScratch scratch;
  int test_argc      = 1;
  char test_argv0[5] = "test";
  char *test_argv[2] = {test_argv0, nullptr};
  std::optional<QApplication> app;

  CaseSetup() {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("XDG_CONFIG_HOME", QByteArray(scratch.config.string().c_str()));
    qputenv("XDG_DATA_HOME", QByteArray(scratch.data.string().c_str()));
    app.emplace(test_argc, test_argv);
    QCoreApplication::setOrganizationName("GameModManager");
    QCoreApplication::setApplicationName("GameModManager");
  }
};

}  // namespace

TEST_CASE("lock_gui off suppresses a plain lock but not an OutputRequired one",
          "[ui]") {
  CaseSetup setup;
  ui::MainWindow w;

  // MO2's stored default: get<bool>(..., "lock_gui", true). Pin it first so a
  // changed default shows up as a failing case rather than silently flipping
  // what "off" means below.
  Settings::instance().set_lock_gui(true);
  REQUIRE(Settings::instance().lock_gui());

  SECTION("lock_gui on: both reasons lock") {
    w.lock_ui(ui::LockReason::LockUI);
    CHECK_FALSE(w.centralWidget()->isEnabled());
    w.unlock_ui();
    CHECK(w.centralWidget()->isEnabled());
  }

  SECTION("lock_gui off: the plain lock does not engage") {
    Settings::instance().set_lock_gui(false);
    w.lock_ui(ui::LockReason::LockUI);
    CHECK(w.centralWidget()->isEnabled());
    w.unlock_ui();
    CHECK(w.centralWidget()->isEnabled());
  }

  SECTION("lock_gui off: an OutputRequired lock STILL engages") {
    Settings::instance().set_lock_gui(false);
    w.lock_ui(ui::LockReason::OutputRequired);
    CHECK_FALSE(w.centralWidget()->isEnabled());
    w.unlock_ui();
    CHECK(w.centralWidget()->isEnabled());
  }
}

TEST_CASE("unlock restores the interface even when lock_gui is off", "[ui]") {
  CaseSetup setup;
  ui::MainWindow w;

  // The gate guards the lock, never the unlock. If it did guard both, turning
  // lock_gui off between lock() and unlock() would leave the window greyed
  // out with no way back but a restart.
  Settings::instance().set_lock_gui(true);
  w.lock_ui(ui::LockReason::OutputRequired);
  REQUIRE_FALSE(w.centralWidget()->isEnabled());

  Settings::instance().set_lock_gui(false);
  w.unlock_ui();
  CHECK(w.centralWidget()->isEnabled());
}

TEST_CASE("unlock leaves a widget the app had already disabled alone", "[ui]") {
  CaseSetup setup;
  ui::MainWindow w;

  // MO2 uilocker.cpp:630: disable() only claims a widget that is currently
  // enabled, so enableAll() cannot undo someone else's deliberate "off".
  w.centralWidget()->setEnabled(false);

  Settings::instance().set_lock_gui(true);
  w.lock_ui(ui::LockReason::LockUI);
  w.unlock_ui();

  CHECK_FALSE(w.centralWidget()->isEnabled());
}