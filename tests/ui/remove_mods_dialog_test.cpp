// Remove Mods confirmation - walking-skeleton adoption of ui::TaskDialog
// (Workspace-52hc). The confirmation is exposed as a testable seam:
//
//     void ui::configure_remove_mods_dialog(TaskDialog& dlg,
//                                           const QStringList& mod_names);
//
// declared in ui/controllers/mod_actions.h and called by
// ModActions::remove_selected_mods() (mod_actions.cpp, the QMessageBox at
// line 131 today). The dialog must carry title "Remove Mods", the count in
// main text, the mod list in the details pane, a Question icon, and exactly
// Yes/No command links; closing it must never confirm (default No).
//
// The source scan below pins the wiring inside remove_selected_mods()
// itself - delete_separator's own QMessageBox::question stays out of scope
// (a future adoption ticket).
//
// TDD red phase: ui::TaskDialog and configure_remove_mods_dialog do not
// exist yet - this fails to compile, which is the expected red.
//
// Hermetic: XDG_CONFIG_HOME under /tmp; no network, no user config access.
// QT_QPA_PLATFORM=offscreen via the test property.
#include "ui/controllers/mod_actions.h"
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

TEST_CASE("remove mods confirmation routes through TaskDialog", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path cfg = "/tmp/gmm_remove_mods_dialog/config";
  std::filesystem::remove_all("/tmp/gmm_remove_mods_dialog");
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
    ui::configure_remove_mods_dialog(dlg, QStringList{"Alpha", "Beta"});
    check(dlg.windowTitle() == "Remove Mods", "seam sets the dialog title");

    auto got = run_dialog(dlg, [](ui::TaskDialog& d) {
      click_link(d, "Yes");
    });
    check(got == QMessageBox::Yes, "Yes confirms the removal");

    auto links = dlg.findChildren<QCommandLinkButton*>();
    check(links.size() == 2, "exactly two choices (Yes / No)");
    if (links.size() == 2) {
      check(links[0]->text() == "Yes", "Yes is the first command link");
      check(links[1]->text() == "No", "No is the second command link");
    }

    bool names_in_details = false;
    for (auto* e : dlg.findChildren<QPlainTextEdit*>()) {
      const QString t = e->toPlainText();
      if (t.contains("Alpha") && t.contains("Beta"))
        names_in_details = true;
    }
    check(names_in_details, "the mod list moves into the details pane");

    bool count_in_main = false;
    for (auto* l : dlg.findChildren<QLabel*>()) {
      if (l->text().contains("2 mod(s)"))
        count_in_main = true;
    }
    check(count_in_main, "main text carries the selected-mod count");
    check(has_icon(dlg), "the confirmation shows the Question icon");
  }

  // ---- No rejects --------------------------------------------------------
  {
    ui::TaskDialog dlg(nullptr, QString());
    ui::configure_remove_mods_dialog(dlg, QStringList{"Alpha"});
    auto got = run_dialog(dlg, [](ui::TaskDialog& d) {
      click_link(d, "No");
    });
    check(got == QMessageBox::No, "No declines the removal");
  }

  // ---- closing never confirms (the old default was No) -------------------
  {
    ui::TaskDialog dlg(nullptr, QString());
    ui::configure_remove_mods_dialog(dlg, QStringList{"Alpha"});
    auto got = run_dialog(dlg, [](ui::TaskDialog& d) {
      d.reject();
    });
    check(got != QMessageBox::Yes,
          "closing the dialog never confirms removal (default No)");
  }

  // ---- remove_selected_mods() is actually wired to the seam --------------
  {
    std::ifstream f(std::string(PROJECT_SOURCE_DIR) +
                    "/src/ui/controllers/mod_actions.cpp");
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string src = ss.str();
    check(!src.empty(), "mod_actions.cpp is readable");

    const std::string needle = "void ModActions::remove_selected_mods()";
    const auto pos           = src.find(needle);
    check(pos != std::string::npos, "remove_selected_mods exists in mod_actions.cpp");
    const auto end = src.find("\nvoid ModActions::", pos + needle.size());
    check(end != std::string::npos, "remove_selected_mods body is bounded");
    const std::string region = src.substr(pos, end - pos);

    check(region.find("TaskDialog") != std::string::npos,
          "the confirmation builds a ui::TaskDialog (not an ad-hoc box)");
    check(region.find("configure_remove_mods_dialog") != std::string::npos,
          "remove_selected_mods routes through the shared configure seam");
    check(region.find("QMessageBox::question") == std::string::npos,
          "the QMessageBox::question confirmation is gone from Remove Mods");
  }
}
