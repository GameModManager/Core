// Settings General tab: "Reset dialog choices" button (Workspace-kxm7).
//
// Closes the TaskDialog remember loop: remembered answers persisted via
// Settings::dialog_choice (MO2 QuestionBoxMemory equivalent) must be
// clearable from the UI, otherwise a remembered wrong answer is permanent.
// MO2 parity: the Reset Dialog Choices control in Settings-General.
//
//   - the Windows group on the General tab exposes a "Reset dialog choices"
//     button (sibling of "Reset dialog sizes and positions")
//   - clicking it clears every stored choice, action-wide AND file-scoped,
//     so TaskDialog asks again
//
// TDD red phase: the button does not exist yet, so the lookup fails - that
// IS the expected red.
//
// Hermetic: throwaway XDG_CONFIG_HOME, no user config access.
// QT_QPA_PLATFORM=offscreen via the test property.
#include "ui/settings/settings.h"
#include "ui/settings/settings_content_widget.h"

#include "engine/pipeline/plugin_host/plugin_loader.h"
#include "engine/platform/theme/theme_manager.h"
#include "ui/theme/style_manager.h"

#include <QApplication>
#include <QMessageBox>
#include <QPushButton>
#include <QTabWidget>
#include <QTimer>

#include <catch2/catch_test_macros.hpp>
#include <filesystem>

namespace {
void check(bool cond, const char *what) {
  INFO(what);
  REQUIRE(cond);
}

// Finds the "Reset dialog choices" button on the General tab (tab 0).
QPushButton *find_reset_choices(ui::SettingsContentWidget &content) {
  auto *general = content.tab_widget()->widget(0);
  if (general == nullptr)
    return nullptr;
  for (auto *btn : general->findChildren<QPushButton *>()) {
    if (btn->text() == "Reset dialog choices")
      return btn;
  }
  return nullptr;
}

// Auto-accepts the confirmation info box the reset button raises, so the
// click below never blocks the suite.
void arm_info_box_closer(QObject *context) {
  QTimer::singleShot(200, context, [] {
    for (auto *w : QApplication::topLevelWidgets()) {
      if (auto *box = qobject_cast<QMessageBox *>(w))
        box->accept();
    }
  });
}

}  // namespace

TEST_CASE("settings reset dialog choices button", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path cfg = "/tmp/gmm_settings_reset_choices/config";
  std::filesystem::remove_all("/tmp/gmm_settings_reset_choices");
  std::filesystem::create_directories(cfg);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  int test_argc     = 1;
  char test_argv0[] = "test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  const std::filesystem::path root = "/tmp/gmm_settings_reset_choices/instances/Test";
  std::filesystem::create_directories(root);

  engine::ThemeManager tm;
  engine::StyleManager style(tm);
  engine::PluginLoader loader;

  ui::SettingsContentWidget content(&style, "breeze", root, &loader);

  // ---- the General tab exposes the reset control -------------------------
  QPushButton *reset_btn = find_reset_choices(content);
  check(reset_btn != nullptr, "General tab has a Reset dialog choices button");

  // ---- clicking it clears every stored choice -----------------------------
  if (reset_btn != nullptr) {
    auto &s = Settings::instance();
    s.set_dialog_choice("gmm_rc_action", "", QMessageBox::Yes);
    s.set_dialog_choice("gmm_rc_action", "readme.ini", QMessageBox::No);
    check(s.dialog_choice("gmm_rc_action", "") == QMessageBox::Yes,
          "an action-wide choice is stored before the reset");
    check(s.dialog_choice("gmm_rc_action", "readme.ini") == QMessageBox::No,
          "a file-scoped choice is stored before the reset");

    arm_info_box_closer(&content);
    reset_btn->click();
    app.processEvents();

    check(!s.dialog_choice("gmm_rc_action", ""), "reset clears the action-wide choice");
    check(!s.dialog_choice("gmm_rc_action", "readme.ini"),
          "reset clears the file-scoped choice");
  }
}
