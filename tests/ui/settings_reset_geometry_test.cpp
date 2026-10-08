// Settings General tab: "Reset dialog sizes and positions" button.
//
// The button tells the user "Dialog sizes and positions have been reset", so it
// has to clear the keys the dialogs actually restore from, and nothing else.
//
// It used to remove() the "geometry" group. Every dialog stores its geometry
// under its own group (fomod/, modinfo/, listdialog/, saveinfo/), so that
// cleared none of them - and the one key actually in the group is
// center_dialogs, so the button silently switched off "Center dialogs on
// screen" while leaving every dialog at the size and place just reset.
//
// Drives the real button on the real General tab. The geometry written below is
// read back through the same getters the four dialogs call in restoreGeometry,
// so a cleared key here means the dialog reopens at its default.
//
// Hermetic: throwaway XDG_CONFIG_HOME, no user config access.
// QT_QPA_PLATFORM=offscreen via the test property.
#include "ui/settings/settings.h"
#include "ui/settings/settings_content_widget.h"

#include "engine/plugin_host/plugin_loader.h"
#include "engine/theme/theme_manager.h"
#include "ui/theme/style_manager.h"

#include <QApplication>
#include <QMessageBox>
#include <QPushButton>
#include <QTabWidget>
#include <QTimer>

#include <catch2/catch_test_macros.hpp>
#include <filesystem>

namespace {
// CHECK, not REQUIRE: the two halves of this bug are independent (the wrong
// keys are cleared, and an unrelated key is destroyed), and REQUIRE aborts the
// case on the first failure - which would hide the second half from every
// negative control run.
void check(bool cond, const char *what) {
  INFO(what);
  CHECK(cond);
}

// The "Reset dialog sizes and positions" button on the General tab (tab 0),
// sibling of "Reset dialog choices".
QPushButton *find_reset_geometry(ui::SettingsContentWidget &content) {
  auto *general = content.tab_widget()->widget(0);
  if (general == nullptr)
    return nullptr;
  for (auto *btn : general->findChildren<QPushButton *>()) {
    if (btn->text() == "Reset dialog sizes and positions")
      return btn;
  }
  return nullptr;
}

// Auto-accepts the confirmation info box the reset button raises, so the click
// below never blocks the suite.
void arm_info_box_closer(QObject *context) {
  QTimer::singleShot(200, context, [] {
    for (auto *w : QApplication::topLevelWidgets()) {
      if (auto *box = qobject_cast<QMessageBox *>(w))
        box->accept();
    }
  });
}

}  // namespace

TEST_CASE("settings reset dialog geometry button", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path cfg = "/tmp/gmm_settings_reset_geometry/config";
  std::filesystem::remove_all("/tmp/gmm_settings_reset_geometry");
  std::filesystem::create_directories(cfg);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  int test_argc     = 1;
  char test_argv0[] = "test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  const std::filesystem::path root = "/tmp/gmm_settings_reset_geometry/instances/Test";
  std::filesystem::create_directories(root);

  engine::ThemeManager tm;
  engine::StyleManager style(tm);
  engine::PluginLoader loader;

  ui::SettingsContentWidget content(&style, "breeze", root, &loader);

  QPushButton *reset_btn = find_reset_geometry(content);
  check(reset_btn != nullptr, "General tab has a Reset dialog sizes button");

  if (reset_btn != nullptr) {
    auto &s = Settings::instance();
    // Written through the same setters the four dialogs save from.
    s.set_fomod_window_geometry(QByteArray("fomod-geom"));
    s.set_modinfo_window_geometry(QByteArray("modinfo-geom"));
    s.set_listdialog_window_geometry(QByteArray("listdialog-geom"));
    s.set_saveinfo_window_geometry(QByteArray("saveinfo-geom"));
    s.set_center_dialogs(true);
    check(!s.fomod_window_geometry().isEmpty() &&
              !s.modinfo_window_geometry().isEmpty() &&
              !s.listdialog_window_geometry().isEmpty() &&
              !s.saveinfo_window_geometry().isEmpty(),
          "all four dialog geometries are stored before the reset");

    arm_info_box_closer(&content);
    reset_btn->click();
    app.processEvents();

    check(s.fomod_window_geometry().isEmpty(),
          "reset clears the FOMOD wizard geometry");
    check(s.modinfo_window_geometry().isEmpty(), "reset clears the Mod Info geometry");
    check(s.listdialog_window_geometry().isEmpty(),
          "reset clears the ListDialog geometry");
    check(s.saveinfo_window_geometry().isEmpty(),
          "reset clears the Save Info geometry");
    // The unrelated boolean that used to live in the group the reset removed.
    check(s.center_dialogs(), "reset leaves Center dialogs on screen alone");
  }

  std::filesystem::remove_all("/tmp/gmm_settings_reset_geometry");
}