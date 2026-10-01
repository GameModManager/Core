// Settings > General > "Confirm before switching game instance".
//
// The setting decides whether an instance switch that has to restart asks
// first. The decision is a free function (instance_settings.h) precisely so it
// can be exercised here without a MainWindow, without a dialog and - crucially
// - without restarting the test process.
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

  // Same effective settings (neither declares an appearance) -> a live switch
  // is safe, so there is nothing to confirm and nothing to restart.
  CHECK(!ui::instance_effective_settings_differ(a, b));
  CHECK(ui::instance_switch_plan(a, b) == ui::InstanceSwitch::Live);

  // A different theme means the already-loaded plugin/theme state would be
  // stale, so the switch must restart - and by default it asks first.
  ui::set_instance_theme(b, QStringLiteral("dark"));
  CHECK(ui::instance_effective_settings_differ(a, b));
  CHECK(s.show_change_game_confirmation());  // the stored default
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
  fs::remove_all(root);
}
