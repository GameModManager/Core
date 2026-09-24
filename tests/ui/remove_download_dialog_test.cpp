// Remove Download confirmation - TaskDialog adoption (Workspace-iqry).
// The confirmation is exposed as a testable seam:
//
//     void ui::configure_remove_download_dialog(TaskDialog& dlg,
//                                               const QString& file_name);
//
// declared in ui/panels/downloads_tab.h and called by the Remove context-menu
// action in DownloadsTab::add_context_menu_actions (downloads_tab.cpp). The
// dialog must carry title "Remove Download", the archive file name in the
// main text, a trash-restore note as content, a Question icon, and exactly
// Yes/No command links; closing it must never confirm (default No).
//
// The source scans below pin the wiring: the menu action routes through the
// seam, while DownloadsTab::remove_entry (the workhorse, also called by the
// rescan vanished-file cleanup in scan_downloads_dir where the file is
// already gone) stays confirm-free.
//
// Hermetic: XDG_CONFIG_HOME under /tmp; no network, no user config access.
// QT_QPA_PLATFORM=offscreen via the test property.
#include "ui/panels/downloads_tab.h"
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
void check(bool cond, const char *what) {
  INFO(what);
  REQUIRE(cond);
}
}  // namespace

namespace {

// See remove_mods_dialog_test.cpp - scripted exec() driving with a watchdog.
void click_link(ui::TaskDialog &dlg, const QString &text) {
  for (auto *b : dlg.findChildren<QCommandLinkButton *>()) {
    if (b->text() == text) {
      b->click();
      return;
    }
  }
  dlg.reject();
}

QMessageBox::StandardButton run_dialog(ui::TaskDialog &dlg,
                                       std::function<void(ui::TaskDialog &)> act) {
  QTimer::singleShot(0, &dlg, [&dlg, act] {
    act(dlg);
  });
  QTimer::singleShot(8000, &dlg, [&dlg] {
    dlg.reject();
  });
  return dlg.exec();
}

bool has_icon(const ui::TaskDialog &dlg) {
  if (!dlg.windowIcon().isNull())
    return true;
  for (auto *l : dlg.findChildren<QLabel *>()) {
    if (!l->pixmap(Qt::ReturnByValue).isNull())
      return true;
  }
  return false;
}

// Returns the body of `needle` (up to the next sibling member function).
std::string function_region(const std::string &src, const std::string &needle,
                            const std::string &scope) {
  const auto pos = src.find(needle);
  if (pos == std::string::npos)
    return {};
  const auto end = src.find(scope, pos + needle.size());
  if (end == std::string::npos)
    return {};
  return src.substr(pos, end - pos);
}

}  // namespace

TEST_CASE("remove download confirmation routes through TaskDialog", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path cfg = "/tmp/gmm_remove_download_dialog/config";
  std::filesystem::remove_all("/tmp/gmm_remove_download_dialog");
  std::filesystem::create_directories(cfg);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  int test_argc     = 1;
  char test_argv0[] = "test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  // ---- the seam builds the confirmation ----------------------------------
  {
    ui::TaskDialog dlg(nullptr, QString());
    ui::configure_remove_download_dialog(dlg, "awesome-mod-1.2.7z");
    check(dlg.windowTitle() == "Remove Download", "seam sets the dialog title");

    auto got = run_dialog(dlg, [](ui::TaskDialog &d) {
      click_link(d, "Yes");
    });
    check(got == QMessageBox::Yes, "Yes confirms the removal");

    auto links = dlg.findChildren<QCommandLinkButton *>();
    check(links.size() == 2, "exactly two choices (Yes / No)");
    if (links.size() == 2) {
      check(links[0]->text() == "Yes", "Yes is the first command link");
      check(links[1]->text() == "No", "No is the second command link");
    }

    bool name_in_main = false;
    for (auto *l : dlg.findChildren<QLabel *>()) {
      if (l->text().contains("awesome-mod-1.2.7z"))
        name_in_main = true;
    }
    check(name_in_main, "main text names the archive file");

    bool trash_in_content = false;
    for (auto *l : dlg.findChildren<QLabel *>()) {
      if (l->text().contains("trash"))
        trash_in_content = true;
    }
    check(trash_in_content, "content notes the trash restore path");
    check(has_icon(dlg), "the confirmation shows the Question icon");
  }

  // ---- No rejects --------------------------------------------------------
  {
    ui::TaskDialog dlg(nullptr, QString());
    ui::configure_remove_download_dialog(dlg, "awesome-mod-1.2.7z");
    auto got = run_dialog(dlg, [](ui::TaskDialog &d) {
      click_link(d, "No");
    });
    check(got == QMessageBox::No, "No declines the removal");
  }

  // ---- closing never confirms (the old behavior had no confirm at all) ---
  {
    ui::TaskDialog dlg(nullptr, QString());
    ui::configure_remove_download_dialog(dlg, "awesome-mod-1.2.7z");
    auto got = run_dialog(dlg, [](ui::TaskDialog &d) {
      d.reject();
    });
    check(got != QMessageBox::Yes,
          "closing the dialog never confirms removal (default No)");
  }

  // ---- the Remove menu action is actually wired to the seam --------------
  {
    std::ifstream f(std::string(PROJECT_SOURCE_DIR) +
                    "/src/ui/panels/downloads_tab.cpp");
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string src = ss.str();
    check(!src.empty(), "downloads_tab.cpp is readable");

    const std::string menu_region = function_region(
        src, "void DownloadsTab::add_context_menu_actions", "\nvoid DownloadsTab::");
    check(!menu_region.empty(), "add_context_menu_actions exists in downloads_tab.cpp");
    check(menu_region.find("TaskDialog") != std::string::npos,
          "the Remove action builds a ui::TaskDialog (not an ad-hoc box)");
    check(menu_region.find("configure_remove_download_dialog") != std::string::npos,
          "the Remove action routes through the shared configure seam");
    check(menu_region.find("remove_entry(id)") != std::string::npos,
          "confirming still drops the row via remove_entry");

    // The workhorse stays confirm-free: the rescan vanished-file cleanup
    // calls remove_entry() for files that are already gone from disk, so a
    // dialog there would nag about nothing.
    const std::string scan_region = function_region(
        src, "void DownloadsTab::scan_downloads_dir", "\nvoid DownloadsTab::");
    check(!scan_region.empty(), "scan_downloads_dir exists in downloads_tab.cpp");
    check(scan_region.find("configure_remove_download_dialog") == std::string::npos,
          "the rescan cleanup path stays silent (no confirm for vanished files)");

    const std::string remove_region = function_region(
        src, "void DownloadsTab::remove_entry", "\nvoid DownloadsTab::");
    check(!remove_region.empty(), "remove_entry exists in downloads_tab.cpp");
    check(remove_region.find("TaskDialog") == std::string::npos,
          "remove_entry itself stays confirm-free (confirm lives in the menu)");
  }
}
