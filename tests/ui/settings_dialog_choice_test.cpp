// Settings::dialog_choice persistence seam for remembered TaskDialog answers
// (Workspace-52hc - MO2 U267 / QuestionBoxMemory equivalent).
//
//   - an unset action returns nullopt (the dialog must ask)
//   - set/get round-trips a QMessageBox::StandardButton, per-action AND
//     per-file granularity (file-scoped choices never leak into the
//     action-wide slot, and vice versa)
//   - setting the same key again overwrites the stored choice
//   - reset_dialog_choices() clears every stored choice
//
// TDD red phase: Settings has no dialog_choice/set_dialog_choice/
// reset_dialog_choices members yet - this fails to compile, which is the
// expected red identifying the missing seam.
//
// Hermetic: throwaway XDG_CONFIG_HOME, no user config access.
#include "ui/settings/settings.h"

#include <QApplication>
#include <QMessageBox>

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <optional>

namespace {
void check(bool cond, const char* what) {
  INFO(what);
  REQUIRE(cond);
}
}  // namespace

TEST_CASE("settings dialog_choice persistence", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path cfg = "/tmp/gmm_settings_dialog_choice/config";
  std::filesystem::remove_all("/tmp/gmm_settings_dialog_choice");
  std::filesystem::create_directories(cfg);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  int test_argc     = 1;
  char test_argv0[] = "test";
  char* test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  auto& s = Settings::instance();

  check(!s.dialog_choice("gmm_sc_unset", ""), "an unset action has no stored choice");

  s.set_dialog_choice("gmm_sc_action", "", QMessageBox::Yes);
  check(s.dialog_choice("gmm_sc_action", "") == QMessageBox::Yes,
        "set/get round-trips the StandardButton");

  s.set_dialog_choice("gmm_sc_action", "readme.ini", QMessageBox::No);
  check(s.dialog_choice("gmm_sc_action", "readme.ini") == QMessageBox::No,
        "a file-scoped choice round-trips");
  check(s.dialog_choice("gmm_sc_action", "") == QMessageBox::Yes,
        "a file-scoped choice does not leak into the action-wide slot");
  check(!s.dialog_choice("gmm_sc_other", ""), "a different action stays unset");

  s.set_dialog_choice("gmm_sc_action", "", QMessageBox::Save);
  check(s.dialog_choice("gmm_sc_action", "") == QMessageBox::Save,
        "setting the same key again overwrites the stored choice");

  s.reset_dialog_choices();
  check(!s.dialog_choice("gmm_sc_action", ""), "reset clears the action-wide choice");
  check(!s.dialog_choice("gmm_sc_action", "readme.ini"),
        "reset clears the file-scoped choice");
}
