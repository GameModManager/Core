// Offscreen GUI regression test for InstallProgressDialog - the MO2-style
// install progress popup. Exercises begin() reset, determinate vs
// indeterminate (busy) bar modes, the non-blocking ApplicationModal show (the
// event loop keeps running so the worker's queued progress signals still
// arrive), and that Escape cannot dismiss it (no cancel by design). Hermetic:
// no file access, QT_QPA_PLATFORM=offscreen via the test property.
#include "ui/install/install_progress_dialog.h"
#include "ui/install/layout_dialog.h"
#include "engine/game/registry/game_features/game_feature_registry.h"
#include "engine/game/registry/game_knowledge.h"

#include <QAction>
#include <QApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QTreeView>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <system_error>
#include <catch2/catch_test_macros.hpp>

namespace {
void check(bool cond, const char *what) {
  INFO(what);
  REQUIRE(cond);
}
}  // namespace

TEST_CASE("install progress dialog", "[ui]") {
  int test_argc     = 1;
  char test_argv0[] = "test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);

  ui::InstallProgressDialog dlg;
  auto *bar   = dlg.findChild<QProgressBar *>();
  auto *label = dlg.findChild<QLabel *>();
  check(bar && label, "dialog has a progress bar and a status label");

  // ApplicationModal: blocks input to the rest of the app while the event
  // loop keeps spinning (progress signals still arrive) - not exec()-modal.
  check(dlg.windowModality() == Qt::ApplicationModal,
        "dialog is ApplicationModal (non-blocking show, blocking input)");

  REQUIRE(bar != nullptr);
  REQUIRE(label != nullptr);

  // begin(): title + empty status + determinate 0%.
  dlg.begin(QStringLiteral("Installing…"));
  check(dlg.windowTitle() == QStringLiteral("Installing…"),
        "begin sets the window title");
  check(label->text().isEmpty(), "begin clears the status label");
  check(bar->maximum() == 100 && bar->value() == 0, "begin resets the bar to 0%");

  // Determinate updates drive the bar and the status line.
  dlg.set_status(QStringLiteral("Extracting mod.zip…"), 42);
  check(label->text() == QStringLiteral("Extracting mod.zip…"),
        "set_status updates the status line");
  check(bar->value() == 42, "set_status drives the bar");

  // Indeterminate stage (archive size unknowable) -> busy bar.
  dlg.set_status(QStringLiteral("Extracting mod.7z…"), -1);
  check(bar->maximum() == 0, "negative percent switches the bar to busy mode");

  // A determinate update switches back.
  dlg.set_status(QStringLiteral("Installing to SkyUI…"), 60);
  check(bar->maximum() == 100 && bar->value() == 60,
        "a determinate update switches the bar back to 0-100");

  // Non-blocking show: the dialog appears without entering a nested event
  // loop, so the worker's queued progress signals still reach the app.
  dlg.show();
  check(dlg.isVisible(), "show() displays the dialog non-blockingly");

  // No cancel: Escape must not dismiss the popup mid-install (MO2 parity).
  QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
  QApplication::sendEvent(&dlg, &esc);
  check(dlg.isVisible(), "Escape does not dismiss the install progress popup");

  dlg.hide();
}

// LayoutDialog - the manual layout dialog the install opens as its LAST resort,
// so it lives with the other install dialogs here. Two things about it are
// load-bearing rather than cosmetic: the verdict follows the subtree the user
// designated (the tree that will actually be installed), and backing out of the
// "Continue?" prompt is the DEFAULT action.
//
// Hermetic: throwaway /tmp tree, no settings access, offscreen platform.
TEST_CASE("layout dialog verdict and continue prompt", "[ui]") {
  int test_argc     = 1;
  char test_argv0[] = "test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);

  const std::filesystem::path root = "/tmp/gmm_layout_dialog";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root / "wrapper" / "meshes");
  std::ofstream(root / "wrapper" / "meshes" / "foo.nif") << "f";
  std::ofstream(root / "readme.txt") << "r";

  engine::GameKnowledge knowledge;
  knowledge.set("fake", "mod_valid_dirs", "meshes");
  knowledge.set("fake", "mod_valid_exts", "");
  auto checker = engine::data_checker_for(knowledge, "fake");
  REQUIRE(checker != nullptr);

  ui::LayoutDialog dlg(root, "Data", checker);
  auto *tree  = dlg.findChild<QTreeView *>();
  auto *model = tree ? tree->model() : nullptr;
  check(tree && model, "the dialog has a tree over the extracted content");

  // Nothing designated = the content root itself: "wrapper" is not a directory
  // the game declared and readme.txt is not a declared extension, so red.
  check(dlg.data_root().empty(), "no subtree is designated to begin with");
  check(dlg.verdict() == engine::LayoutVerdict::Invalid, "the junk root is red");
  check(dlg.verdict_text().contains(QStringLiteral("does not look valid")),
        "the label says so in words, not just in colour");

  // Right-clicking a folder and taking "Set as <data> directory" designates it;
  // the verdict is recomputed against THAT subtree, which here holds meshes/.
  // The dialog opens the pseudo-root up front, so its children exist.
  const auto wrapper_index = model->index(0, 0, model->index(0, 0));
  REQUIRE(wrapper_index.isValid());
  QAction *set_action   = nullptr;
  QAction *unset_action = nullptr;
  for (auto *action : dlg.findChildren<QAction *>()) {
    if (action->text().contains(QStringLiteral("Set as")))
      set_action = action;
    else if (action->text().contains(QStringLiteral("Unset")))
      unset_action = action;
  }
  check(set_action && unset_action,
        "the context menu offers Set as / Unset <data> directory");
  check(set_action->text().contains(QStringLiteral("<data>")),
        "the action names the game's data directory");
  tree->setCurrentIndex(wrapper_index);
  set_action->trigger();
  check(dlg.data_root() == std::filesystem::path("wrapper"),
        "Set as designates the chosen subtree");
  check(dlg.verdict() == engine::LayoutVerdict::Valid,
        "the designated subtree is green");

  // Unset reverts to the root, and with it the red verdict.
  unset_action->trigger();
  check(dlg.data_root().empty(), "Unset drops the designation");
  check(dlg.verdict() == engine::LayoutVerdict::Invalid,
        "unsetting reverts the verdict to the root's");

  // No declaration at all is a third answer, not a red one: nothing was
  // checked, so nothing is claimed.
  ui::LayoutDialog unchecked(root, "Data", nullptr);
  check(unchecked.verdict() == engine::LayoutVerdict::Unknown,
        "a game that declared nothing cannot be checked");
  check(unchecked.verdict_text().contains(QStringLiteral("Cannot check")),
        "the label says it cannot check rather than that it is invalid");

  // OK on a red verdict puts up "Continue?" - and Cancel is the default, so a
  // stray Enter backs out instead of installing a layout the game cannot read.
  std::unique_ptr<QMessageBox> prompt(ui::LayoutDialog::make_continue_prompt(&dlg));
  REQUIRE(prompt != nullptr);
  auto *def = prompt->defaultButton();
  check(def != nullptr && def == prompt->button(QMessageBox::Cancel),
        "the Cancel button is the DEFAULT button of the Continue prompt");
  check(def && def->text() == QStringLiteral("Cancel"),
        "the default reads Cancel, so Enter backs out");

  std::error_code ec;
  std::filesystem::remove_all(root, ec);
}
