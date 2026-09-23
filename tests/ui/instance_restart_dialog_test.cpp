// Instance-switch restart confirmation - TaskDialog adoption of the MO2 U044
// flow (Workspace-nef3). The confirmation is exposed as a testable seam:
//
//     void ui::configure_instance_restart_dialog(TaskDialog& dlg);
//
// declared in ui/controllers/settings_controller.h and called by
// SettingsController::switch_to_instance() (settings_controller.cpp) when the
// target instance's effective settings differ from the current ones. The
// dialog must carry title/main "Restart GameModManager", the
// must-restart-to-finish-configuration-changes content, a Question icon, and
// exactly Restart(Yes) / Continue(No, "Some things might be weird.")
// command links - MO2 mainwindow.cpp:2768-2780, modulo the app name. There
// is no remember row: MO2 asks every time here.
//
// Reply semantics pinned below: Restart confirms the restart path,
// Continue takes the live in-process switch, and closing the dialog aborts
// the switch (neither path).
//
// The source scan pins the wiring inside switch_to_instance() itself - the
// silent auto-restart (w_->close + restart_application with no prompt) is
// the bug being fixed.
//
// Hermetic: XDG_CONFIG_HOME under /tmp; no network, no user config access.
// QT_QPA_PLATFORM=offscreen via the test property.
#include "ui/controllers/settings_controller.h"
#include "ui/widgets/task_dialog.h"

#include <QApplication>
#include <QCommandLinkButton>
#include <QLabel>
#include <QMessageBox>
#include <QTimer>

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>

namespace {
void check(bool cond, const char* what) {
  INFO(what);
  REQUIRE(cond);
}
}  // namespace

namespace {

// See task_dialog_test.cpp - scripted exec() driving with a watchdog.
void click_link(ui::TaskDialog& dlg, const QString& text) {
  for (auto* b : dlg.findChildren<QCommandLinkButton*>()) {
    if (b->text() == text) {
      b->click();
      return;
    }
  }
  dlg.reject();
}

QMessageBox::StandardButton run_dialog(ui::TaskDialog& dlg,
                                       std::function<void(ui::TaskDialog&)> act) {
  QTimer::singleShot(0, &dlg, [&dlg, act] {
    act(dlg);
  });
  QTimer::singleShot(8000, &dlg, [&dlg] {
    dlg.reject();
  });
  return dlg.exec();
}

bool has_icon(const ui::TaskDialog& dlg) {
  if (!dlg.windowIcon().isNull())
    return true;
  for (auto* l : dlg.findChildren<QLabel*>()) {
    if (!l->pixmap(Qt::ReturnByValue).isNull())
      return true;
  }
  return false;
}

}  // namespace

TEST_CASE("instance switch restart confirmation routes through TaskDialog", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path cfg = "/tmp/gmm_instance_restart_dialog/config";
  std::filesystem::remove_all("/tmp/gmm_instance_restart_dialog");
  std::filesystem::create_directories(cfg);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  int test_argc     = 1;
  char test_argv0[] = "test";
  char* test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  // ---- the seam builds the confirmation ----------------------------------
  {
    ui::TaskDialog dlg(nullptr, QString());
    ui::configure_instance_restart_dialog(dlg);
    check(dlg.windowTitle() == "Restart GameModManager", "seam sets the dialog title");

    auto got = run_dialog(dlg, [](ui::TaskDialog& d) {
      click_link(d, "Restart");
    });
    check(got == QMessageBox::Yes, "Restart confirms the restart path");

    auto links = dlg.findChildren<QCommandLinkButton*>();
    check(links.size() == 2, "exactly two choices (Restart / Continue)");
    if (links.size() == 2) {
      check(links[0]->text() == "Restart", "Restart is the first command link");
      check(links[1]->text() == "Continue", "Continue is the second command link");
      check(links[1]->description() == "Some things might be weird.",
            "Continue carries the MO2 caveat as its description");
    }

    bool content_set = false;
    bool main_set    = false;
    for (auto* l : dlg.findChildren<QLabel*>()) {
      if (l->text().contains("must restart to finish configuration changes"))
        content_set = true;
      if (l->text() == "Restart GameModManager")
        main_set = true;
    }
    check(content_set, "content carries the must-restart explanation");
    check(main_set, "main text carries the restart instruction");
    check(has_icon(dlg), "the confirmation shows the Question icon");
  }

  // ---- Continue takes the live switch ------------------------------------
  {
    ui::TaskDialog dlg(nullptr, QString());
    ui::configure_instance_restart_dialog(dlg);
    auto got = run_dialog(dlg, [](ui::TaskDialog& d) {
      click_link(d, "Continue");
    });
    check(got == QMessageBox::No, "Continue takes the live-switch path");
  }

  // ---- closing aborts the switch (neither path) --------------------------
  {
    ui::TaskDialog dlg(nullptr, QString());
    ui::configure_instance_restart_dialog(dlg);
    auto got = run_dialog(dlg, [](ui::TaskDialog& d) {
      d.reject();
    });
    check(got != QMessageBox::Yes && got != QMessageBox::No,
          "closing the dialog takes neither the restart nor the live path");
  }

  // ---- switch_to_instance() is actually wired to the seam ----------------
  {
    std::ifstream f(std::string(PROJECT_SOURCE_DIR) +
                    "/src/ui/controllers/settings_controller.cpp");
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string src = ss.str();
    check(!src.empty(), "settings_controller.cpp is readable");

    const std::string needle = "bool SettingsController::switch_to_instance";
    const auto pos           = src.find(needle);
    check(pos != std::string::npos, "switch_to_instance exists");
    const auto end = src.find("\nvoid SettingsController::", pos + needle.size());
    check(end != std::string::npos, "switch_to_instance body is bounded");
    const std::string region = src.substr(pos, end - pos);

    check(region.find("TaskDialog") != std::string::npos,
          "the restart decision builds a ui::TaskDialog (not a silent restart)");
    check(region.find("configure_instance_restart_dialog") != std::string::npos,
          "switch_to_instance routes through the shared configure seam");
    check(region.find("QMessageBox::question") == std::string::npos,
          "no ad-hoc QMessageBox::question on the restart path");
  }
}
