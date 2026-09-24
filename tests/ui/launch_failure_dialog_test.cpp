// Launch-failure details dialogs - TaskDialog adoption (Workspace-t373).
// The two spawn-failure acknowledgments in
// LaunchController::on_launch_params_prepared() (launch_controller.cpp) are
// exposed as testable seams:
//
//     void ui::configure_executable_unreachable_dialog(TaskDialog& dlg,
//                                                      const QString& exec_path);
//     void ui::configure_launch_failed_dialog(TaskDialog& dlg,
//                                             const QString& exec_path);
//
// declared in ui/controllers/launch_controller.h. Both keep the exact
// historical wording, re-tiered into the TaskDialog 3-tier hierarchy: the
// former flat text becomes main (+ content), and the executable path moves
// into the expandable details pane instead of being inlined in the message.
// Both are acknowledgment-only (no command links - the dialog falls back to
// a single plain Ok) with a Warning icon (parity with the QMessageBox::warning
// calls they replace).
//
// Wording pinned below (from the pre-adoption QMessageBox calls):
//  - unreachable: main "The selected executable is not reachable in the game
//    directory.", content "If it belongs to a mod, make sure that mod is
//    enabled.", details = the executable path.
//  - failed: main "Failed to launch game.", details = the executable path.
//
// The source scan pins the wiring inside on_launch_params_prepared() itself:
// both failure exits route through the shared configure seams, and no
// ad-hoc QMessageBox::warning remains on those paths.
//
// Hermetic: XDG_CONFIG_HOME under /tmp; no network, no user config access.
// QT_QPA_PLATFORM=offscreen via the test property.
#include "ui/controllers/launch_controller.h"
#include "ui/widgets/task_dialog.h"

#include <QApplication>
#include <QCommandLinkButton>
#include <QDialogButtonBox>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
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

// See task_dialog_test.cpp - scripted exec() driving with a watchdog.
void click_ok(ui::TaskDialog &dlg) {
  if (auto *box = dlg.findChild<QDialogButtonBox *>()) {
    if (auto *ok = box->button(QDialogButtonBox::Ok)) {
      ok->click();
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

bool label_contains(const ui::TaskDialog &dlg, const QString &text) {
  for (auto *l : dlg.findChildren<QLabel *>()) {
    if (l->text().contains(text))
      return true;
  }
  return false;
}

bool details_contain(const ui::TaskDialog &dlg, const QString &text) {
  for (auto *e : dlg.findChildren<QPlainTextEdit *>()) {
    if (e->toPlainText().contains(text))
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

TEST_CASE("executable unreachable failure routes through TaskDialog", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path cfg = "/tmp/gmm_launch_failure_dialog/config";
  std::filesystem::remove_all("/tmp/gmm_launch_failure_dialog");
  std::filesystem::create_directories(cfg);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  int test_argc     = 1;
  char test_argv0[] = "test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  const QString exe = "/games/skyrim/SkyrimSE.exe";

  // ---- the seam builds the acknowledgment ---------------------------------
  {
    ui::TaskDialog dlg(nullptr, QString());
    ui::configure_executable_unreachable_dialog(dlg, exe);
    check(dlg.windowTitle() == "Launch", "seam sets the dialog title");

    auto got = run_dialog(dlg, [](ui::TaskDialog &d) {
      click_ok(d);
    });
    check(got == QMessageBox::Ok, "Ok acknowledges the failure");

    auto links = dlg.findChildren<QCommandLinkButton *>();
    check(links.isEmpty(), "no command links (acknowledgment-only failure)");

    check(label_contains(dlg, "not reachable in the game directory"),
          "main text keeps the unreachable wording");
    check(label_contains(dlg, "make sure that mod is enabled"),
          "content keeps the mod-enabled hint");
    check(details_contain(dlg, exe), "details pane carries the executable path");
    check(has_icon(dlg), "the failure shows the Warning icon");
  }

  // ---- closing dismisses (same as Ok - pure acknowledgment) ----------------
  {
    ui::TaskDialog dlg(nullptr, QString());
    ui::configure_executable_unreachable_dialog(dlg, exe);
    auto got = run_dialog(dlg, [](ui::TaskDialog &d) {
      d.reject();
    });
    check(got == QMessageBox::Cancel, "closing dismisses the acknowledgment");
  }
}

TEST_CASE("failed launch failure routes through TaskDialog", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path cfg = "/tmp/gmm_launch_failure_dialog/config";
  std::filesystem::remove_all("/tmp/gmm_launch_failure_dialog");
  std::filesystem::create_directories(cfg);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  int test_argc     = 1;
  char test_argv0[] = "test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  const QString exe = "/games/skyrim/SkyrimSE.exe";

  // ---- the seam builds the acknowledgment ---------------------------------
  {
    ui::TaskDialog dlg(nullptr, QString());
    ui::configure_launch_failed_dialog(dlg, exe);
    check(dlg.windowTitle() == "Launch", "seam sets the dialog title");

    auto got = run_dialog(dlg, [](ui::TaskDialog &d) {
      click_ok(d);
    });
    check(got == QMessageBox::Ok, "Ok acknowledges the failure");

    auto links = dlg.findChildren<QCommandLinkButton *>();
    check(links.isEmpty(), "no command links (acknowledgment-only failure)");

    check(label_contains(dlg, "Failed to launch game"),
          "main text keeps the historical wording");
    check(details_contain(dlg, exe), "details pane carries the executable path");
    check(has_icon(dlg), "the failure shows the Warning icon");
  }

  // ---- closing dismisses (same as Ok - pure acknowledgment) ----------------
  {
    ui::TaskDialog dlg(nullptr, QString());
    ui::configure_launch_failed_dialog(dlg, exe);
    auto got = run_dialog(dlg, [](ui::TaskDialog &d) {
      d.reject();
    });
    check(got == QMessageBox::Cancel, "closing dismisses the acknowledgment");
  }

  // ---- on_launch_params_prepared() is actually wired to the seams ---------
  {
    std::ifstream f(std::string(PROJECT_SOURCE_DIR) +
                    "/src/ui/controllers/launch_controller.cpp");
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string src = ss.str();
    check(!src.empty(), "launch_controller.cpp is readable");

    const std::string region =
        function_region(src, "void LaunchController::on_launch_params_prepared",
                        "\nvoid LaunchController::");
    check(!region.empty(), "on_launch_params_prepared exists in launch_controller.cpp");
    check(region.find("TaskDialog") != std::string::npos,
          "the failure exits build a ui::TaskDialog (not an ad-hoc box)");
    check(region.find("configure_executable_unreachable_dialog") != std::string::npos,
          "the unreachable exit routes through the shared configure seam");
    check(region.find("configure_launch_failed_dialog") != std::string::npos,
          "the spawn-failure exit routes through the shared configure seam");
    check(region.find("QMessageBox::warning") == std::string::npos,
          "no ad-hoc QMessageBox::warning remains on the failure paths");
  }
}
