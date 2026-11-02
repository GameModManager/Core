// Offscreen GUI test for the Settings > Experimental tab (Workspace-kmi2).
//
// Verifies:
//   - the Experimental tab exists,
//   - it contains both the nested mod list toggle and the guided instance
//     creation wizard toggle,
//   - toggling the wizard checkbox flips Settings::instance_creation_wizard().
//
// Hermetic: offscreen platform, throwaway XDG_CONFIG_HOME, no instance.
#include "ui/settings/settings.h"
#include "ui/settings/settings_content_widget.h"

#include <QApplication>
#include <QCheckBox>
#include <QTabWidget>

#include <catch2/catch_test_macros.hpp>

namespace {

int test_argc = 1;
char test_argv0[] = "settings_experimental_tab_test";
char *test_argv[] = {test_argv0};

}  // namespace

TEST_CASE("settings experimental tab exists with both controls",
          "[ui][settings][experimental]") {
  QApplication app(test_argc, test_argv);

  ui::SettingsContentWidget w(nullptr, QString(), std::filesystem::path(), nullptr);
  auto *tabs = w.tab_widget();
  REQUIRE(tabs != nullptr);

  int idx = -1;
  for (int i = 0; i < tabs->count(); ++i) {
    if (tabs->tabText(i) == QStringLiteral("Experimental")) {
      idx = i;
      break;
    }
  }
  REQUIRE(idx >= 0);

  // Both controls present somewhere in the widget tree.
  auto boxes = w.findChildren<QCheckBox *>();
  bool has_nested = false;
  bool has_wizard = false;
  for (auto *b : boxes) {
    if (b->text() == QStringLiteral("Nested mod list ⚠️ (Experimental)"))
      has_nested = true;
    if (b->text() == QStringLiteral("Use guided instance creation wizard"))
      has_wizard = true;
  }
  REQUIRE(has_nested);
  REQUIRE(has_wizard);

  // Toggle wizard checkbox persists to the setting.
  QCheckBox *wizard_box = nullptr;
  for (auto *b : boxes) {
    if (b->text() == QStringLiteral("Use guided instance creation wizard")) {
      wizard_box = b;
      break;
    }
  }
  REQUIRE(wizard_box != nullptr);

  Settings::instance().set_instance_creation_wizard(false);
  REQUIRE(Settings::instance().instance_creation_wizard() == false);
  wizard_box->setChecked(true);
  QCoreApplication::processEvents();
  REQUIRE(Settings::instance().instance_creation_wizard() == true);

  wizard_box->setChecked(false);
  QCoreApplication::processEvents();
  REQUIRE(Settings::instance().instance_creation_wizard() == false);
}
