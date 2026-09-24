// Offscreen GUI test for the reusable ui::report_error / ui::critical_on_top
// global error popup (Workspace-so1s, MO2 U268 port). Contract source: the
// MO2 report.cpp spec captured in the Workspace-52hc analysis note -
// reportError(msg) logs + shows a modal error popup, criticalOnTop shows a
// raised (stays-on-top) critical popup when no main window is around.
//
//   - report_error routes title/message/details into a Critical-icon dialog
//     titled "Error" (pinned through the presenter seam, no modal UI).
//   - The default presenter shows the dialog modally; a scripted Ok click
//     dismisses it (offscreen, nullptr parent = the pre-GUI path).
//   - critical_on_top adds Qt::WindowStaysOnTopHint (the raised-modal part)
//     while keeping the Critical icon and the message.
//   - Clearing the presenter restores the default modal presentation.
//
// TDD red phase: src/ui/widgets/error_popup.{h,cpp} does not exist yet, so
// the include below fails the build - that IS the expected red.
//
// Hermetic: XDG_CONFIG_HOME under /tmp; no network, no user config access.
// QT_QPA_PLATFORM=offscreen via the test property.
#include "ui/widgets/error_popup.h"

#include "ui/widgets/task_dialog.h"

#include <QApplication>
#include <QCommandLinkButton>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTimer>

#include <catch2/catch_test_macros.hpp>
#include <filesystem>

namespace {
void check(bool cond, const char *what) {
  INFO(what);
  REQUIRE(cond);
}

// Presses whatever dismisses the dialog: the Ok fallback button or the
// first command link. Falls back to reject() so exec() always unblocks.
void dismiss(ui::TaskDialog &dlg) {
  if (auto *box = dlg.findChild<QDialogButtonBox *>()) {
    if (auto *b = box->button(QDialogButtonBox::Ok)) {
      b->click();
      return;
    }
  }
  auto links = dlg.findChildren<QCommandLinkButton *>();
  if (!links.isEmpty()) {
    links.front()->click();
    return;
  }
  auto buttons = dlg.findChildren<QPushButton *>();
  if (!buttons.isEmpty()) {
    buttons.front()->click();
    return;
  }
  dlg.reject();
}

bool has_critical_icon(const ui::TaskDialog &dlg) {
  if (!dlg.windowIcon().isNull())
    return true;
  for (auto *l : dlg.findChildren<QLabel *>()) {
    if (!l->pixmap(Qt::ReturnByValue).isNull())
      return true;
  }
  return false;
}

bool label_shows(const ui::TaskDialog &dlg, const QString &text) {
  for (auto *l : dlg.findChildren<QLabel *>()) {
    if (l->text() == text)
      return true;
  }
  return false;
}

}  // namespace

TEST_CASE("error popup", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path cfg = "/tmp/gmm_error_popup/config";
  std::filesystem::remove_all("/tmp/gmm_error_popup");
  std::filesystem::create_directories(cfg);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  int test_argc     = 1;
  char test_argv0[] = "test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  // ---- report_error routes title/message/details into the dialog ---------
  {
    QString seen_title;
    QString seen_main;
    bool seen_critical = false;
    ui::set_error_presenter_for_tests([&](ui::TaskDialog &dlg) {
      seen_title    = dlg.windowTitle();
      seen_main     = label_shows(dlg, "Failed to terminate process group 42: boom")
                          ? QString("ok")
                          : QString();
      seen_critical = has_critical_icon(dlg);
      dismiss(dlg);
    });
    ui::report_error("Failed to terminate process group 42: boom");
    ui::set_error_presenter_for_tests(nullptr);
    check(seen_title == "Error", "report_error titles the dialog Error");
    check(seen_main == "ok", "report_error shows the message as the main instruction");
    check(seen_critical, "report_error uses the Critical icon");
  }

  // ---- details land in the collapsible details pane ----------------------
  {
    QString seen_details;
    ui::set_error_presenter_for_tests([&](ui::TaskDialog &dlg) {
      for (auto *e : dlg.findChildren<QPlainTextEdit *>()) {
        if (e->toPlainText() == "line one\nline two")
          seen_details = e->toPlainText();
      }
      dismiss(dlg);
    });
    ui::report_error("Something broke", nullptr, "line one\nline two");
    ui::set_error_presenter_for_tests(nullptr);
    check(seen_details == "line one\nline two",
          "report_error puts details in the collapsible pane");
  }

  // ---- default presenter shows a modal dialog a scripted click closes ----
  {
    bool closed = false;
    QTimer::singleShot(200, [&] {
      for (QWidget *w : QApplication::topLevelWidgets()) {
        if (w->windowTitle() == "Error" && w->isVisible()) {
          if (auto *dlg = dynamic_cast<ui::TaskDialog *>(w)) {
            dismiss(*dlg);
            closed = true;
            return;
          }
        }
      }
    });
    // Watchdog: closing everything triggers closeEvent -> reject(), so a
    // missing dialog fails the assertion instead of hanging the suite.
    QTimer::singleShot(8000, [] {
      QApplication::closeAllWindows();
    });
    ui::report_error("Modal probe");
    check(closed, "the default presenter shows a modal Error dialog");
  }

  // ---- critical_on_top stays on top with the Critical icon ---------------
  {
    bool on_top        = false;
    bool critical      = false;
    bool message_shown = false;
    ui::set_error_presenter_for_tests([&](ui::TaskDialog &dlg) {
      on_top        = dlg.windowFlags() & Qt::WindowStaysOnTopHint;
      critical      = has_critical_icon(dlg);
      message_shown = label_shows(dlg, "Raised probe");
      dismiss(dlg);
    });
    ui::critical_on_top("Raised probe");
    ui::set_error_presenter_for_tests(nullptr);
    check(on_top, "critical_on_top sets WindowStaysOnTopHint (raised modal)");
    check(critical, "critical_on_top keeps the Critical icon");
    check(message_shown, "critical_on_top shows the message");
  }
}
