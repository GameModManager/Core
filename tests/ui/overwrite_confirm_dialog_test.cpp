// Overwrite confirmations - TaskDialog adoption (Workspace-t6z5).
// Two raw QMessageBox decision points move onto the shared component,
// each exposed as a testable seam:
//
//     void ui::configure_clear_overwrite_dialog(TaskDialog& dlg);
//     void ui::configure_overwrite_delete_dialog(TaskDialog& dlg, int count,
//                                                const QString& file_name);
//
// declared in ui/controllers/overwrite_controller.h and
// ui/overwrite/overwrite_info_dialog.h, called by
// OverwriteController::clear_overwrite and
// OverwriteInfoDialog::delete_selected respectively. Wording is preserved
// word-for-word from the replaced QMessageBox calls (the trailing trash
// sentence moves to content, per the Workspace-iqry split); closing either
// dialog never confirms (default No).
//
// Deliberately NOT converted (see the analysis note on Workspace-t6z5):
// the move-to-mod picker (MoveToModDialog) and the sync picker
// (SyncOverwriteDialog) contain no QMessageBox decision points - only
// information/warning result boxes, which stay QMessageBox per the 52hc
// program rule - and QueryOverwriteDialog (Merge/Replace/Rename/Cancel) is
// a custom QDialog, not a raw QMessageBox point. The source scans below
// pin that scoping.
//
// Hermetic: XDG_CONFIG_HOME under /tmp; no network, no user config access.
// QT_QPA_PLATFORM=offscreen via the test property.
#include "ui/controllers/overwrite_controller.h"
#include "ui/overwrite/overwrite_info_dialog.h"
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

bool label_contains(const ui::TaskDialog &dlg, const QString &text) {
  for (auto *l : dlg.findChildren<QLabel *>()) {
    if (l->text().contains(text))
      return true;
  }
  return false;
}

}  // namespace

TEST_CASE("overwrite confirmations route through TaskDialog", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path cfg = "/tmp/gmm_overwrite_confirm_dialog/config";
  std::filesystem::remove_all("/tmp/gmm_overwrite_confirm_dialog");
  std::filesystem::create_directories(cfg);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  int test_argc     = 1;
  char test_argv0[] = "test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  // ---- clear-overwrite seam builds the confirmation -----------------------
  {
    ui::TaskDialog dlg(nullptr, QString());
    ui::configure_clear_overwrite_dialog(dlg);
    check(dlg.windowTitle() == "Clear Overwrite", "seam sets the dialog title");

    auto got = run_dialog(dlg, [](ui::TaskDialog &d) {
      click_link(d, "Yes");
    });
    check(got == QMessageBox::Yes, "Yes confirms the clear");

    auto links = dlg.findChildren<QCommandLinkButton *>();
    check(links.size() == 2, "exactly two choices (Yes / No)");
    if (links.size() == 2) {
      check(links[0]->text() == "Yes", "Yes is the first command link");
      check(links[1]->text() == "No", "No is the second command link");
    }

    check(label_contains(dlg, "Overwrite"), "main text names the Overwrite folder");
    check(label_contains(dlg, "trash"), "content notes the trash restore path");
    check(has_icon(dlg), "the confirmation shows the Question icon");
  }

  // ---- No / close never clears --------------------------------------------
  {
    ui::TaskDialog dlg(nullptr, QString());
    ui::configure_clear_overwrite_dialog(dlg);
    auto got = run_dialog(dlg, [](ui::TaskDialog &d) {
      click_link(d, "No");
    });
    check(got == QMessageBox::No, "No declines the clear");
  }
  {
    ui::TaskDialog dlg(nullptr, QString());
    ui::configure_clear_overwrite_dialog(dlg);
    auto got = run_dialog(dlg, [](ui::TaskDialog &d) {
      d.reject();
    });
    check(got != QMessageBox::Yes,
          "closing the dialog never confirms the clear (default No)");
  }

  // ---- overwrite delete seam: single file ---------------------------------
  {
    ui::TaskDialog dlg(nullptr, QString());
    ui::configure_overwrite_delete_dialog(dlg, 1, "foo.nif");

    auto got = run_dialog(dlg, [](ui::TaskDialog &d) {
      click_link(d, "Yes");
    });
    check(got == QMessageBox::Yes, "Yes confirms the single-file delete");

    auto links = dlg.findChildren<QCommandLinkButton *>();
    check(links.size() == 2, "single delete offers exactly Yes / No");
    check(label_contains(dlg, "foo.nif"), "single main text names the file");
    check(label_contains(dlg, "trash"), "single content notes the trash path");
    check(has_icon(dlg), "the single delete shows the Question icon");
  }

  // ---- overwrite delete seam: multiple entries -----------------------------
  {
    ui::TaskDialog dlg(nullptr, QString());
    ui::configure_overwrite_delete_dialog(dlg, 3, QString());

    auto got = run_dialog(dlg, [](ui::TaskDialog &d) {
      click_link(d, "Yes");
    });
    check(got == QMessageBox::Yes, "Yes confirms the multi delete");
    check(label_contains(dlg, "3"), "multi main text names the entry count");
    check(label_contains(dlg, "trash"), "multi content notes the trash path");
  }

  // ---- No / close never deletes --------------------------------------------
  {
    ui::TaskDialog dlg(nullptr, QString());
    ui::configure_overwrite_delete_dialog(dlg, 2, QString());
    auto got = run_dialog(dlg, [](ui::TaskDialog &d) {
      click_link(d, "No");
    });
    check(got == QMessageBox::No, "No declines the delete");
  }
  {
    ui::TaskDialog dlg(nullptr, QString());
    ui::configure_overwrite_delete_dialog(dlg, 1, "foo.nif");
    auto got = run_dialog(dlg, [](ui::TaskDialog &d) {
      d.reject();
    });
    check(got != QMessageBox::Yes,
          "closing the dialog never confirms the delete (default No)");
  }

  // ---- clear_overwrite is actually wired to the seam -----------------------
  {
    std::ifstream f(std::string(PROJECT_SOURCE_DIR) +
                    "/src/ui/controllers/overwrite_controller.cpp");
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string src = ss.str();
    check(!src.empty(), "overwrite_controller.cpp is readable");

    const std::string clear_region =
        function_region(src, "void OverwriteController::clear_overwrite",
                        "\nvoid OverwriteController::");
    check(!clear_region.empty(), "clear_overwrite exists in overwrite_controller.cpp");
    check(clear_region.find("TaskDialog") != std::string::npos,
          "clear_overwrite builds a ui::TaskDialog (not an ad-hoc box)");
    check(clear_region.find("configure_clear_overwrite_dialog") != std::string::npos,
          "clear_overwrite routes through the shared configure seam");
    check(clear_region.find("QMessageBox::question") == std::string::npos,
          "clear_overwrite no longer asks via a raw QMessageBox");
    check(clear_region.find("engine::clear_overwrite") != std::string::npos,
          "confirming still clears via engine::clear_overwrite");
  }

  // ---- delete_selected is actually wired to the seam ------------------------
  {
    std::ifstream f(std::string(PROJECT_SOURCE_DIR) +
                    "/src/ui/overwrite/overwrite_info_dialog.cpp");
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string src = ss.str();
    check(!src.empty(), "overwrite_info_dialog.cpp is readable");

    const std::string delete_region =
        function_region(src, "void OverwriteInfoDialog::delete_selected",
                        "\nvoid OverwriteInfoDialog::");
    check(!delete_region.empty(),
          "delete_selected exists in overwrite_info_dialog.cpp");
    check(delete_region.find("TaskDialog") != std::string::npos,
          "delete_selected builds a ui::TaskDialog (not an ad-hoc box)");
    check(delete_region.find("configure_overwrite_delete_dialog") != std::string::npos,
          "delete_selected routes through the shared configure seam");
    check(delete_region.find("QMessageBox::question") == std::string::npos,
          "delete_selected no longer asks via a raw QMessageBox");
    check(delete_region.find("engine::remove_path") != std::string::npos,
          "confirming still trashes via engine::remove_path");
  }

  // ---- move-to-mod / sync pickers stay as-is (no decision points) -----------
  {
    std::ifstream f(std::string(PROJECT_SOURCE_DIR) +
                    "/src/ui/controllers/overwrite_controller.cpp");
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string src = ss.str();
    check(!src.empty(), "overwrite_controller.cpp is readable (move/sync pin)");

    const std::string move_region =
        function_region(src, "void OverwriteController::move_overwrite_content_to_mod",
                        "\nvoid OverwriteController::");
    check(!move_region.empty(), "move_overwrite_content_to_mod exists");
    check(move_region.find("TaskDialog") == std::string::npos,
          "the move-to-mod picker stays a ListDialog (no decision point there)");

    const std::string sync_region =
        function_region(src, "void OverwriteController::sync_overwrite_to_mods",
                        "\nvoid OverwriteController::");
    check(!sync_region.empty(), "sync_overwrite_to_mods exists");
    check(sync_region.find("TaskDialog") == std::string::npos,
          "the sync picker stays a SyncOverwriteDialog (no decision point there)");
  }
}
