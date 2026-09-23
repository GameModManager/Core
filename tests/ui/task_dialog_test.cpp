// Offscreen GUI test for the reusable ui::TaskDialog component (Workspace-52hc,
// MO2 U267 port). Contract source: the analysis note on Workspace-52hc.
//
//   - The fluent builder chain (title/main/content/details/icon/add_button/
//     add_content/remember/set_minimum_width) compiles and populates the
//     dialog; each method returns the dialog itself.
//   - exec() maps every added button to its QMessageBox::StandardButton id;
//     rejecting (X/Escape) returns Cancel; with no added buttons the dialog
//     falls back to a single plain Ok standard button.
//   - Command-link rendering: added buttons all render as QCommandLinkButton
//     in insertion order, a description becomes the command-link subtitle,
//     an empty description stays a plain command link; no added buttons at
//     all means no command links (QDialogButtonBox-Ok path instead).
//   - remember(): a stored choice short-circuits exec() without running the
//     dialog logic; a picked answer persists into Settings::dialog_choice;
//     clearing the stored choice makes the dialog ask again.
//
// TDD red phase: src/ui/widgets/task_dialog.{h,cpp} does not exist yet, so
// the include below fails the build - that IS the expected red. The
// Settings::dialog_choice seam is exercised through the same file.
//
// Hermetic: XDG_CONFIG_HOME under /tmp; no network, no user config access.
// QT_QPA_PLATFORM=offscreen via the test property.
#include "ui/widgets/task_dialog.h"

#include "ui/settings/settings.h"

#include <QApplication>
#include <QCommandLinkButton>
#include <QDialogButtonBox>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QTimer>

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <functional>
#include <optional>

namespace {
void check(bool cond, const char* what) {
  INFO(what);
  REQUIRE(cond);
}
}  // namespace

namespace {

// Clicks the command-link button with the given text. When the scripted
// button cannot be found the dialog is rejected, so exec() unblocks and the
// result assertion fails instead of the test hanging.
void click_link(ui::TaskDialog& dlg, const QString& text) {
  for (auto* b : dlg.findChildren<QCommandLinkButton*>()) {
    if (b->text() == text) {
      b->click();
      return;
    }
  }
  dlg.reject();
}

// Clicks the Ok button of the standard button box (no-custom-buttons path).
void click_ok(ui::TaskDialog& dlg) {
  auto* box = dlg.findChild<QDialogButtonBox*>();
  if (box) {
    if (auto* b = box->button(QDialogButtonBox::Ok)) {
      b->click();
      return;
    }
  }
  dlg.reject();
}

// Runs dlg.exec() with a scripted interaction armed for the first turn of the
// event loop, plus a watchdog that rejects after 8s so a broken dialog can
// never hang the suite.
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

// The dialog shows its icon either as the window icon or as a pixmapped
// label (style standard icon in the left icon panel) - accept both
// placements so the assertion pins "an icon is shown", not its container.
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

TEST_CASE("task dialog", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path cfg = "/tmp/gmm_task_dialog/config";
  std::filesystem::remove_all("/tmp/gmm_task_dialog");
  std::filesystem::create_directories(cfg);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  int test_argc     = 1;
  char test_argv0[] = "test";
  char* test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  // ---- fluent builder populates the dialog --------------------------------
  {
    ui::TaskDialog dlg(nullptr, "Builder");
    auto* injected = new QLabel("Injected content");
    injected->setObjectName("injected_content");
    ui::TaskDialog& chain = dlg.title("Builder")
                                .main("Main instruction")
                                .content("Body content")
                                .details("Detailed\ndump")
                                .icon(QMessageBox::Question)
                                .add_content(injected)
                                .remember("gmm_td_builder")
                                .set_minimum_width(600);
    check(&chain == &dlg, "builder methods return the dialog itself (fluent)");

    // No custom buttons -> the Ok fallback path completes exec().
    auto result = run_dialog(dlg, [](ui::TaskDialog& d) {
      click_ok(d);
    });
    check(result == QMessageBox::Ok, "button-box Ok completes the dialog");

    check(dlg.windowTitle() == "Builder", "title() sets the window title");
    check(dlg.minimumWidth() == 600, "set_minimum_width sets the minimum width");
    check(dlg.isAncestorOf(injected), "add_content parents the widget into the dialog");

    bool main_found    = false;
    bool content_found = false;
    bool details_found = false;
    for (auto* l : dlg.findChildren<QLabel*>()) {
      if (l->text() == "Main instruction")
        main_found = true;
      if (l->text() == "Body content")
        content_found = true;
    }
    check(main_found, "main() text is shown in a label");
    check(content_found, "content() text is shown in a label");
    for (auto* e : dlg.findChildren<QPlainTextEdit*>()) {
      if (e->toPlainText() == "Detailed\ndump")
        details_found = true;
    }
    check(details_found, "details() text is shown in the details pane");
    check(has_icon(dlg), "icon() shows the Question icon");

    // remember() is wired even on the Ok path: the answer was persisted.
    auto stored = Settings::instance().dialog_choice("gmm_td_builder", "");
    check(stored == QMessageBox::Ok, "remember() persists the picked answer");
  }

  // ---- exec() maps each added button to its StandardButton ---------------
  {
    const QMessageBox::StandardButton ids[] = {
        QMessageBox::Yes,  QMessageBox::No,     QMessageBox::Ok,   QMessageBox::Cancel,
        QMessageBox::Save, QMessageBox::Ignore, QMessageBox::Retry};
    for (auto id : ids) {
      CAPTURE(id);
      ui::TaskDialog dlg(nullptr, "Map");
      dlg.add_button({"Probe", "", id});
      auto got = run_dialog(dlg, [](ui::TaskDialog& d) {
        click_link(d, "Probe");
      });
      check(got == id, "the clicked button's StandardButton id comes back");
    }
  }

  // ---- rejecting returns Cancel ------------------------------------------
  {
    ui::TaskDialog dlg(nullptr, "Reject");
    dlg.add_button({"Yes", "", QMessageBox::Yes});
    auto got = run_dialog(dlg, [](ui::TaskDialog& d) {
      d.reject();
    });
    check(got == QMessageBox::Cancel, "rejecting (X/Escape) returns Cancel");
  }

  // ---- command links vs plain button box ---------------------------------
  {
    ui::TaskDialog dlg(nullptr, "Links");
    dlg.add_button({"Yes", "Move files to the trash", QMessageBox::Yes})
        .add_button({"No", "", QMessageBox::No});
    auto got = run_dialog(dlg, [](ui::TaskDialog& d) {
      click_link(d, "Yes");
    });
    check(got == QMessageBox::Yes, "first command link maps to Yes");

    auto links = dlg.findChildren<QCommandLinkButton*>();
    check(links.size() == 2, "every added button renders as a command link");
    if (links.size() == 2) {
      check(links[0]->text() == "Yes", "command links keep insertion order (first)");
      check(links[1]->text() == "No", "command links keep insertion order (second)");
      check(links[0]->description() == "Move files to the trash",
            "a description renders as the command-link subtitle");
      check(links[1]->description().isEmpty(),
            "an empty description stays a plain command link");
    }
  }
  {
    ui::TaskDialog dlg(nullptr, "Plain");
    auto got = run_dialog(dlg, [](ui::TaskDialog& d) {
      click_ok(d);
    });
    check(got == QMessageBox::Ok, "no added buttons falls back to Ok");
    check(dlg.findChildren<QCommandLinkButton*>().isEmpty(),
          "no added buttons means no command links");
  }

  // ---- remember: the picked answer is persisted --------------------------
  {
    Settings::instance().reset_dialog_choices();
    ui::TaskDialog dlg(nullptr, "Remember");
    dlg.remember("gmm_td_pick")
        .add_button({"Yes", "", QMessageBox::Yes})
        .add_button({"No", "", QMessageBox::No});
    bool asked = false;
    auto got   = run_dialog(dlg, [&asked](ui::TaskDialog& d) {
      asked = true;
      click_link(d, "Yes");
    });
    check(got == QMessageBox::Yes, "the picked answer is returned");
    check(asked, "the dialog asked (no stored choice yet)");
    auto stored = Settings::instance().dialog_choice("gmm_td_pick", "");
    check(stored == QMessageBox::Yes, "remember() stored the answer");
  }

  // ---- remember: a stored choice short-circuits without asking ----------
  {
    Settings::instance().set_dialog_choice("gmm_td_stored", "", QMessageBox::No);
    ui::TaskDialog dlg(nullptr, "ShortCircuit");
    dlg.remember("gmm_td_stored")
        .add_button({"Yes", "", QMessageBox::Yes})
        .add_button({"No", "", QMessageBox::No});
    bool asked = false;
    auto got   = run_dialog(dlg, [&asked](ui::TaskDialog& d) {
      asked = true;          // only runs if the dialog actually opened
      click_link(d, "Yes");  // ...and would flip the answer to Yes
    });
    check(got == QMessageBox::No, "the stored choice is returned untouched");
    check(!asked, "the dialog logic never ran (no ask)");
    check(!dlg.isVisible(), "the dialog was never shown");
    auto stored = Settings::instance().dialog_choice("gmm_td_stored", "");
    check(stored == QMessageBox::No,
          "short-circuit does not overwrite the stored choice");
  }

  // ---- reset: clearing dialog_choice makes the dialog ask again ---------
  {
    Settings::instance().reset_dialog_choices();
    check(!Settings::instance().dialog_choice("gmm_td_pick", ""),
          "reset_dialog_choices clears stored choices");

    ui::TaskDialog dlg(nullptr, "AfterReset");
    dlg.remember("gmm_td_pick")
        .add_button({"Yes", "", QMessageBox::Yes})
        .add_button({"No", "", QMessageBox::No});
    bool asked = false;
    auto got   = run_dialog(dlg, [&asked](ui::TaskDialog& d) {
      asked = true;
      click_link(d, "No");
    });
    check(asked, "the dialog asks again after a reset");
    check(got == QMessageBox::No, "the new pick is returned");
    auto stored = Settings::instance().dialog_choice("gmm_td_pick", "");
    check(stored == QMessageBox::No, "the new pick is re-persisted");
  }
}
