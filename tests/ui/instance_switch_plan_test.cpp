// Settings > General > "Restart when switching game instance" (default on)
// and "Confirm before switching game instance".
//
// Together they decide what an instance switch does. The decision is a free
// function (instance_settings.h) precisely so it can be exercised here
// without a MainWindow, without a dialog and - crucially - without restarting
// the test process.
#include "ui/settings/instance_settings.h"
#include "ui/settings/settings.h"

#include <QApplication>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

namespace fs = std::filesystem;

TEST_CASE("instance switch plan follows the confirmation setting", "[ui][settings]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const fs::path root = fs::temp_directory_path() / "gmm_switch_plan";
  fs::remove_all(root);
  fs::create_directories(root / "config");
  fs::create_directories(root / "a");
  fs::create_directories(root / "b");
  qputenv("XDG_CONFIG_HOME", (root / "config").string().c_str());
  int argc     = 1;
  char argv0[] = "test";
  char *argv[] = {argv0, nullptr};
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  auto &s          = Settings::instance();
  const fs::path a = root / "a";
  const fs::path b = root / "b";

  // The restart-only default: every switch restarts, whatever the two
  // instances' effective settings are. With the confirmation setting off
  // the restart just happens.
  CHECK(s.restart_on_instance_switch());  // the stored default
  s.set_show_change_game_confirmation(false);
  CHECK(ui::instance_switch_plan(a, b) == ui::InstanceSwitch::Restart);
  CHECK(ui::instance_switch_plan(a, a) == ui::InstanceSwitch::Restart);

  // Confirmation on: the restart asks first.
  s.set_show_change_game_confirmation(true);
  CHECK(s.show_change_game_confirmation());
  CHECK(ui::instance_switch_plan(a, b) == ui::InstanceSwitch::Ask);

  // OFF fallback: today's rules. Same effective settings -> a live switch is
  // safe, so there is nothing to confirm and nothing to restart.
  s.set_restart_on_instance_switch(false);
  CHECK(!ui::instance_effective_settings_differ(a, b));
  CHECK(ui::instance_switch_plan(a, b) == ui::InstanceSwitch::Live);

  // A different theme means the already-loaded plugin/theme state would be
  // stale, so the switch must restart - and by default it asks first.
  ui::set_instance_theme(b, QStringLiteral("dark"));
  CHECK(ui::instance_effective_settings_differ(a, b));
  CHECK(ui::instance_switch_plan(a, b) == ui::InstanceSwitch::Ask);

  // The setting's real effect: off, the same switch restarts without asking.
  s.set_show_change_game_confirmation(false);
  CHECK(!s.show_change_game_confirmation());
  CHECK(ui::instance_switch_plan(a, b) == ui::InstanceSwitch::Restart);

  // Off, but nothing needs restarting: still a live switch. Turning the
  // confirmation off must never turn a live switch into a restart.
  CHECK(ui::instance_switch_plan(a, a) == ui::InstanceSwitch::Live);

  s.set_show_change_game_confirmation(true);
  CHECK(ui::instance_switch_plan(a, b) == ui::InstanceSwitch::Ask);
  s.set_restart_on_instance_switch(true);
  CHECK(ui::instance_switch_plan(a, b) == ui::InstanceSwitch::Ask);
  fs::remove_all(root);
}
