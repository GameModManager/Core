// View > Show Menu Bar must have a way back.
//
// The toggle hides the whole bar, and the View menu lives IN the bar - so
// unticking it can leave the window with no menus at all. MO2 does not rely on
// the window manager for this: MainWindow::keyReleaseEvent
// (references/modorganizer/src/mainwindow.cpp:4054-4068) re-shows a hidden
// menu bar on an Alt key RELEASE, gated on the showMenubarOnAlt setting.
//
// Driven through the REAL window, not by calling the handler: the restore is a
// key event override, so the only honest check is Qt routing an Alt release to
// it - QTest::keyRelease aimed at the window, the same gesture a user makes.
//
// Hermetic: offscreen platform, throwaway XDG_CONFIG_HOME, no game, no
// instance, no files touched.
#include "ui/main_window/main_window.h"
#include "ui/widgets/menu_bar.h"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QMenu>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

TEST_CASE("a hidden menu bar comes back on an Alt key release", "[ui][menu]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path root = "/tmp/opencode/gmm_menu_bar_restore/config";
  std::filesystem::remove_all("/tmp/opencode/gmm_menu_bar_restore");
  std::filesystem::create_directories(root);
  qputenv("XDG_CONFIG_HOME", root.c_str());

  int test_argc     = 1;
  char test_argv0[] = "test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  ui::MainWindow w;
  w.show();
  QCoreApplication::processEvents();

  auto *bar = w.findChild<ui::AppMenuBar *>();
  REQUIRE(bar != nullptr);
  REQUIRE(bar->isVisible());

  QAction *toggle = nullptr;
  for (QAction *act : bar->actions()) {
    if (act->menu() != nullptr && act->menu()->title() == "&View")
      for (QAction *inner : act->menu()->actions())
        if (inner->text() == "Show Menu Bar")
          toggle = inner;
  }
  REQUIRE(toggle != nullptr);
  REQUIRE(toggle->isChecked());

  // Hide it the way the user does. No key event brings it back - the toggle is
  // in the bar it just hid, so the Alt release below is the only way out.
  toggle->setChecked(false);
  REQUIRE_FALSE(bar->isVisible());
  REQUIRE_FALSE(toggle->isChecked());

  QTest::keyRelease(&w, Qt::Key_Alt);
  QCoreApplication::processEvents();

  CHECK(bar->isVisible());
  // The checkbox follows the bar: leaving it unticked while the bar is on
  // screen is the state that reads as "broken" to the user.
  CHECK(toggle->isChecked());

  std::filesystem::remove_all("/tmp/opencode/gmm_menu_bar_restore");
}