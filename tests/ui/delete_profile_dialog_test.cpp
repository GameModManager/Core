// Delete Profile confirmation - TaskDialog adoption (Workspace-iqry).
// The confirmation is exposed as a testable seam:
//
//     void ui::configure_delete_profile_dialog(TaskDialog& dlg,
//                                              const QString& profile_name,
//                                              const QString& profile_dir);
//
// declared in ui/profile/profile_manager_dialog.h and called by
// ProfileManagerDialog::on_delete_profile (profile_manager_dialog.cpp,
// replacing the QMessageBox::question there). The dialog must carry title
// "Delete Profile", the profile name plus the removal warning in the main
// text, a cannot-be-undone note as content (profile removal is permanent),
// the profile directory path in the details pane, a Question icon, and
// exactly Yes/No command links; closing it must never confirm (default No).
//
// The active-profile guard warning above the seam stays a QMessageBox - out
// of scope for this adoption.
//
// Hermetic: XDG_CONFIG_HOME under /tmp; no network, no user config access.
// QT_QPA_PLATFORM=offscreen via the test property.
#include "ui/profile/profile_manager_dialog.h"
#include "ui/widgets/task_dialog.h"

#include <QApplication>
#include <QCommandLinkButton>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
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

// See remove_mods_dialog_test.cpp - scripted exec() driving with a watchdog.
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

TEST_CASE("delete profile confirmation routes through TaskDialog", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path cfg = "/tmp/gmm_delete_profile_dialog/config";
  std::filesystem::remove_all("/tmp/gmm_delete_profile_dialog");
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
    ui::configure_delete_profile_dialog(dlg, "Survival",
                                        "/instances/main/profiles/Survival");
    check(dlg.windowTitle() == "Delete Profile", "seam sets the dialog title");

    auto got = run_dialog(dlg, [](ui::TaskDialog& d) {
      click_link(d, "Yes");
    });
    check(got == QMessageBox::Yes, "Yes confirms the deletion");

    auto links = dlg.findChildren<QCommandLinkButton*>();
    check(links.size() == 2, "exactly two choices (Yes / No)");
    if (links.size() == 2) {
      check(links[0]->text() == "Yes", "Yes is the first command link");
      check(links[1]->text() == "No", "No is the second command link");
    }

    bool name_in_main = false;
    for (auto* l : dlg.findChildren<QLabel*>()) {
      if (l->text().contains("Survival"))
        name_in_main = true;
    }
    check(name_in_main, "main text names the profile");

    bool dir_in_details = false;
    for (auto* e : dlg.findChildren<QPlainTextEdit*>()) {
      if (e->toPlainText().contains("/instances/main/profiles/Survival"))
        dir_in_details = true;
    }
    check(dir_in_details, "the profile dir path moves into the details pane");
    check(has_icon(dlg), "the confirmation shows the Question icon");
  }

  // ---- No rejects --------------------------------------------------------
  {
    ui::TaskDialog dlg(nullptr, QString());
    ui::configure_delete_profile_dialog(dlg, "Survival", "/profiles/Survival");
    auto got = run_dialog(dlg, [](ui::TaskDialog& d) {
      click_link(d, "No");
    });
    check(got == QMessageBox::No, "No declines the deletion");
  }

  // ---- closing never confirms (the old default was No) -------------------
  {
    ui::TaskDialog dlg(nullptr, QString());
    ui::configure_delete_profile_dialog(dlg, "Survival", "/profiles/Survival");
    auto got = run_dialog(dlg, [](ui::TaskDialog& d) {
      d.reject();
    });
    check(got != QMessageBox::Yes,
          "closing the dialog never confirms deletion (default No)");
  }

  // ---- on_delete_profile() is actually wired to the seam -----------------
  {
    std::ifstream f(std::string(PROJECT_SOURCE_DIR) +
                    "/src/ui/profile/profile_manager_dialog.cpp");
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string src = ss.str();
    check(!src.empty(), "profile_manager_dialog.cpp is readable");

    const std::string needle = "void ProfileManagerDialog::on_delete_profile";
    const auto pos           = src.find(needle);
    check(pos != std::string::npos,
          "on_delete_profile exists in profile_manager_dialog.cpp");
    const auto end = src.find("\nvoid ProfileManagerDialog::", pos + needle.size());
    check(end != std::string::npos, "on_delete_profile body is bounded");
    const std::string region = src.substr(pos, end - pos);

    check(region.find("TaskDialog") != std::string::npos,
          "the confirmation builds a ui::TaskDialog (not an ad-hoc box)");
    check(region.find("configure_delete_profile_dialog") != std::string::npos,
          "on_delete_profile routes through the shared configure seam");
    check(region.find("QMessageBox::question") == std::string::npos,
          "the QMessageBox::question confirmation is gone from Delete Profile");
  }
}
