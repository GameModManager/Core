// Settings > Windows > "Center dialogs on screen".
//
// The setting only means something once a dialog has restored its remembered
// geometry, so the case drives the real placement helper and asserts where the
// dialog ends up - not that the getter returns what was set.
#include "ui/settings/settings.h"
#include "ui/widgets/dialog_placement.h"

#include <QApplication>
#include <QDialog>
#include <QGuiApplication>
#include <QScreen>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

TEST_CASE("center dialogs on screen", "[ui][settings]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path cfg = "/tmp/gmm_center_dialogs/config";
  std::filesystem::remove_all("/tmp/gmm_center_dialogs");
  std::filesystem::create_directories(cfg);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  int test_argc     = 1;
  char test_argv0[] = "test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  auto *screen = QGuiApplication::primaryScreen();
  REQUIRE(screen != nullptr);
  const auto area = screen->availableGeometry();

  // Placed in a corner, as a remembered geometry would leave it.
  const QPoint corner(area.left() + 3, area.top() + 7);
  QDialog dlg;
  dlg.resize(220, 140);
  dlg.move(corner);
  REQUIRE(dlg.pos() == corner);

  SECTION("off (the default): the restored position is left alone") {
    Settings::instance().set_center_dialogs(false);
    center_dialog_on_screen(&dlg);
    CHECK(dlg.pos() == corner);
  }

  SECTION("on: the dialog is re-centred on its screen") {
    Settings::instance().set_center_dialogs(true);
    center_dialog_on_screen(&dlg);
    // 1px tolerance: the helper positions from size()/2, which rounds for an
    // odd-sized dialog.
    const auto centre = dlg.geometry().center();
    CHECK(std::abs(centre.x() - area.center().x()) <= 1);
    CHECK(std::abs(centre.y() - area.center().y()) <= 1);
  }

  SECTION("a null dialog is a no-op, not a crash") {
    Settings::instance().set_center_dialogs(true);
    center_dialog_on_screen(nullptr);
  }

  Settings::instance().set_center_dialogs(false);
  std::filesystem::remove_all("/tmp/gmm_center_dialogs");
}
