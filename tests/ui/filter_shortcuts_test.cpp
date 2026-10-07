// Filter-bar MO2 parity.
//
//   Placeholder - the filter input's is MO2's plain "Filter", not
//         "Filter..." (references/modorganizer/src/mainwindow.ui:584,997).
//   Shortcuts - Ctrl+F focuses the active filter input from anywhere in the main
//         window; Escape clears it and ALWAYS hands focus back to the owning
//         list, whether or not there was text to clear. MO2 wires exactly this
//         pair in setFilterShortcuts() (mainwindow.cpp:204-232) and calls it
//         for the mod list, the plugin list and the downloads list
//         (mainwindow.cpp:464-466).
//
// Asserted through the REAL window, not by poking the handler: the shortcuts
// are QShortcuts parented to MainWindow, so the only honest check is Qt
// actually routing the key event to them - a QTest::keyClick aimed at the
// window, the same gesture a user performs.
//
// Hermetic: offscreen platform, no game and no instance, no files touched.
// Rows go straight into the model so the clear path is observable end to end
// (text -> apply_mod_filter -> row visibility) instead of inferred.
#include "ui/main_window/main_window.h"
#include "ui/widgets/mod_filter_bar.h"
#include "ui/widgets/mod_list_model.h"
#include "ui/widgets/mod_table_view.h"
#include "ui/widgets/right_filter_bar.h"

#include <QApplication>
#include <QCoreApplication>
#include <QLineEdit>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

namespace {

// The bars own their line edit privately; the widget tree is the only public
// door to it, and it is the same one the user reaches.
QLineEdit *filter_input(ui::ModFilterBar *bar) {
  return bar == nullptr ? nullptr : bar->findChild<QLineEdit *>();
}

QLineEdit *filter_input(ui::RightFilterBar *bar) {
  return bar == nullptr ? nullptr : bar->findChild<QLineEdit *>();
}

// Rows the user can see right now - "the filter really was applied" and "the
// clear really was applied" are both observable, not assumed.
int visible_rows(ui::ModView *view) {
  int count = 0;
  for (int row = 0; row < view->model()->rowCount(); ++row)
    count += view->isRowHidden(row, QModelIndex()) ? 0 : 1;
  return count;
}

}  // namespace

TEST_CASE("filter bar: Ctrl+F focuses the filter, Escape clears it", "[ui][filter]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path root = "/tmp/opencode/gmm_sds7_filter/config";
  std::filesystem::remove_all("/tmp/opencode/gmm_sds7_filter");
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
  w.activateWindow();
  QCoreApplication::processEvents();

  auto *mod_bar    = w.findChild<ui::ModFilterBar *>();
  auto *right_bar  = w.findChild<ui::RightFilterBar *>();
  auto *mod_edit   = filter_input(mod_bar);
  auto *right_edit = filter_input(right_bar);
  REQUIRE(mod_bar != nullptr);
  REQUIRE(right_bar != nullptr);
  REQUIRE(mod_edit != nullptr);
  REQUIRE(right_edit != nullptr);
  auto *view = w.mod_view();
  REQUIRE(view != nullptr);
  auto *model = qobject_cast<ui::ModList *>(view->model());
  REQUIRE(model != nullptr);

  model->add_mod("SkyrimHD", "Skyrim HD Textures", "1.0", 50);
  model->add_mod("Uno", "Uno Something", "2.0", 40);
  const int all_rows = visible_rows(view);
  REQUIRE(all_rows >= 2);

  SECTION("G52: both filter inputs carry MO2's plain Filter placeholder") {
    CHECK(mod_edit->placeholderText() == "Filter");
    CHECK(right_edit->placeholderText() == "Filter");
  }

  SECTION("Ctrl+F focuses the filter input and selects its text from anywhere") {
    view->setFocus();
    REQUIRE(view->hasFocus());
    mod_edit->setText("Sky");
    QTest::keyClick(&w, Qt::Key_F, Qt::ControlModifier);
    CHECK(mod_edit->hasFocus());
    CHECK(mod_edit->selectedText() == "Sky");
  }

  SECTION("Escape clears the filter and hands focus back to the mod list") {
    mod_edit->setText("Sky");  // hides "Uno Something", leaves one row
    CHECK(visible_rows(view) < all_rows);
    mod_edit->setFocus();
    QTest::keyClick(&w, Qt::Key_Escape);
    CHECK(mod_edit->text().isEmpty());
    CHECK(visible_rows(view) == all_rows);
    CHECK(view->hasFocus());
  }

  SECTION("Escape on an already-empty filter still hands focus to the mod list") {
    // Focus is IN the filter, the filter is EMPTY, Escape must still hand
    // focus back and the input must not keep the keyboard. MO2's reset lambda
    // is unconditional (edit->clear(); widget->setFocus();), so the hand-back
    // must not depend on there having been text to clear.
    mod_edit->setFocus();
    REQUIRE(mod_edit->hasFocus());
    REQUIRE(mod_edit->text().isEmpty());
    QTest::keyClick(&w, Qt::Key_Escape);
    CHECK(mod_edit->text().isEmpty());
    CHECK(view->hasFocus());
  }

  SECTION("Ctrl+F follows focus into the right-panel filter") {
    right_edit->setText("Sky");
    right_edit->setFocus();
    REQUIRE(right_edit->hasFocus());
    QTest::keyClick(&w, Qt::Key_F, Qt::ControlModifier);
    CHECK(right_edit->hasFocus());
    CHECK(right_edit->selectedText() == "Sky");
  }

  SECTION("Escape clears the right-panel filter without leaving the panel") {
    right_edit->setText("Sky");
    right_edit->setFocus();
    REQUIRE(right_edit->hasFocus());
    QTest::keyClick(&w, Qt::Key_Escape);
    CHECK(right_edit->text().isEmpty());  // proves the shortcut fired
    CHECK(right_edit->hasFocus());
    // The deliberate right-panel deviation: there is no single owning view to
    // hand focus to, so Escape must NOT jump across the window to the mod list.
    CHECK_FALSE(view->hasFocus());
  }

  SECTION("Escape leaves the other pane's filter alone") {
    // The shortcut pair is scoped to the pane the focus is in
    // (Qt::WidgetWithChildrenShortcut, MO2 mainwindow.cpp:220,225). A
    // window-scoped pair is the papercut: typing nowhere near the mod list
    // and pressing Escape would wipe its filter and drag focus across the
    // window. Asserted through real key routing, the same gesture the user
    // makes.
    mod_edit->setText("Sky");
    const int filtered = visible_rows(view);
    REQUIRE(filtered < all_rows);
    right_edit->setFocus();
    REQUIRE(right_edit->hasFocus());
    QTest::keyClick(&w, Qt::Key_Escape);
    CHECK(mod_edit->text() == "Sky");       // untouched
    CHECK(visible_rows(view) == filtered);  // and so are the rows
    CHECK_FALSE(view->hasFocus());          // focus did not jump panes
  }

  std::filesystem::remove_all("/tmp/opencode/gmm_sds7_filter");
}
