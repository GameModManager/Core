// The Instance Options toolbar button is a SPLIT button: the body opens the
// Instance Options panel, the side arrow opens the dropdown menu.
//
// Regression under test: commit 905aca50 switched the button to
// QToolButton::InstantPopup once a menu was attached, citing MO2's
// setupActionMenu (references/modorganizer/src/mainwindow.cpp:746-753). Qt
// documents InstantPopup as "the menu is displayed, without delay, when the
// tool button is pressed. In this mode, the button's own action is NOT
// triggered", so the body click stopped opening the panel and the panel became
// reachable only through the menu's "Install recommended packages" entry.
//
// Driven through the REAL widget and REAL mouse events
// (QApplication::sendEvent). QToolButton::click() is not enough: it is
// QAbstractButton::click(), which never sees the popup mode, and would report
// a body click under every mode including the broken one. The mode is read
// off QToolButton::mousePressEvent, which is where the regression lives.
//
// NOT covered: the arrow half (a press in the menu area opening the menu).
// It cannot be driven headless - QToolButton::showMenu() BLOCKS under the
// offscreen platform, while a bare QMenu::popup() on the same parent does
// not (measured, see the report). What is asserted instead is the property
// Qt documents for that half: "MenuButtonPopup ... the menu is displayed when
// the arrow part of the button is pressed", with a menu actually attached.
#include "ui/widgets/main_toolbar.h"

#include <QApplication>
#include <QIcon>
#include <QMenu>
#include <QMouseEvent>
#include <QToolButton>

#include <catch2/catch_test_macros.hpp>
#include <filesystem>

namespace {
void check(bool cond, const char *what) {
  INFO(what);
  REQUIRE(cond);
}

// A left press+release at `x` inside `button`, delivered the way a real mouse
// does (QApplication::sendEvent, not the synthetic click() shortcut).
void press_and_release(QToolButton *button, int x) {
  const QPointF local(x, button->height() / 2.0);
  const QPointF global = button->mapToGlobal(QPoint(x, button->height() / 2));
  QApplication::sendEvent(button, new QMouseEvent(QEvent::MouseButtonPress, local,
                                                  global, Qt::LeftButton,
                                                  Qt::LeftButton, Qt::NoModifier));
  QApplication::sendEvent(button, new QMouseEvent(QEvent::MouseButtonRelease, local,
                                                  global, Qt::LeftButton, Qt::NoButton,
                                                  Qt::NoModifier));
}

// The toolbar's Instance Options button, found the way the window finds it:
// the tooltip is the only thing that identifies it among the toolbar's
// buttons.
QToolButton *instance_options_button(ui::MainToolbar &toolbar) {
  for (auto *b : toolbar.findChildren<QToolButton *>()) {
    if (b->toolTip() == QLatin1String("Instance Options"))
      return b;
  }
  return nullptr;
}
}  // namespace

TEST_CASE("instance options toolbar button is a split button", "[ui][toolbar]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path cfg = "/tmp/gmm_toolbar_split/config";
  std::filesystem::remove_all("/tmp/gmm_toolbar_split");
  std::filesystem::create_directories(cfg);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  // QSettings itself is sandboxed once in tests/support/test_main.cpp.
  int test_argc     = 1;
  char test_argv0[] = "test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  ui::MainToolbar toolbar;
  toolbar.add_instance_options_button(QIcon());
  toolbar.show();
  QApplication::processEvents();

  auto *button = instance_options_button(toolbar);
  REQUIRE(button != nullptr);

  int panel_opens = 0;
  QObject::connect(&toolbar, &ui::MainToolbar::instance_options_clicked,
                   [&panel_opens]() {
                     panel_opens++;
                   });

  // The panel entry point has to be live BEFORE any menu exists, so a build
  // order or an early exit that never attaches one cannot silently strand it.
  check(button->popupMode() == QToolButton::MenuButtonPopup,
        "a menu-less button is a plain button, so it opens the panel");
  press_and_release(button, 1);
  QApplication::processEvents();
  check(panel_opens == 1, "a body click opens the Instance Options panel");

  auto *menu = new QMenu(&toolbar);
  menu->addAction(QStringLiteral("Run winecfg"));
  toolbar.set_instance_options_menu(menu);
  check(button->menu() == menu, "the arrow half needs a menu to open");

  // The behaviour the mode below protects: with a menu attached, a click on the
  // BODY (the left edge, well clear of the menu area Qt reserves on the right)
  // still reaches the consumer - MainWindow connects this signal to
  // TabModeController::route_instance_options (main_window.cpp:161-162). Under
  // the regressed InstantPopup mode this click never reaches the assertion: it
  // is swallowed by showMenu() and, headless, blocks there forever.
  press_and_release(button, 1);
  QApplication::processEvents();
  check(panel_opens == 2,
        "a body click still opens the panel once the dropdown menu is attached");

  // The regression itself. Asserted after the click above on purpose, so the
  // failing mode is reported as a clean red and not as the block it causes.
  check(button->popupMode() == QToolButton::MenuButtonPopup,
        "attaching a menu must not turn the button into InstantPopup, whose "
        "whole contract is that the body click no longer fires the action");

  // Detaching the menu must not cost the body half either.
  toolbar.set_instance_options_menu(nullptr);
  check(button->popupMode() == QToolButton::MenuButtonPopup,
        "removing the menu must not change the popup mode");
  press_and_release(button, 1);
  QApplication::processEvents();
  check(panel_opens == 3,
        "with no menu attached the whole button is body, and it still opens "
        "the panel");

  toolbar.hide();
  std::filesystem::remove_all("/tmp/gmm_toolbar_split");
}