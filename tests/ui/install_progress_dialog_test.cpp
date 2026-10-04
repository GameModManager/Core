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
#include <QCoreApplication>
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

// The tree has to be able to REACH the data, not just show the first level. An
// archive whose content sits inside a nested folder has its game data at depth
// 2 or 3, so a directory the user cannot open is data they cannot designate and
// the dialog has no answer left to give.
//
// Exercised on the MODEL, not on pixels: hasChildren is the exact call
// QTreeView reads to decide whether to draw an expander at all, and
// canFetchMore/fetchMore is the lazy population behind it.
//
// Hermetic: throwaway /tmp tree, no settings access, offscreen platform.
TEST_CASE("layout dialog tree reaches nested directories", "[ui]") {
  int test_argc     = 1;
  char test_argv0[] = "test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);

  const std::filesystem::path root = "/tmp/gmm_layout_tree";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root / "wrapper" / "textures" / "deep");
  std::ofstream(root / "wrapper" / "textures" / "deep" / "gizmo.dds") << "d";
  std::filesystem::create_directories(root / "wrapper" / "hollow");
  std::ofstream(root / "wrapper" / "readme.txt") << "r";
  // A wide level, because an archive holds tens of thousands of entries and
  // asking whether a directory has children must not cost reading them.
  std::filesystem::create_directories(root / "wide");
  for (int i = 0; i < 500; ++i)
    std::ofstream(root / "wide" / ("f" + std::to_string(i) + ".txt")) << "x";

  engine::GameKnowledge knowledge;
  knowledge.set("fake", "mod_valid_dirs", "textures");
  knowledge.set("fake", "mod_valid_exts", "");
  auto checker = engine::data_checker_for(knowledge, "fake");
  REQUIRE(checker != nullptr);

  ui::LayoutDialog dlg(root, "Data", "CoolMod.rar", checker);
  auto *tree  = dlg.findChild<QTreeView *>();
  auto *model = tree ? tree->model() : nullptr;
  check(tree && model, "the dialog has a tree over the extracted content");

  const auto child_named = [model](const QModelIndex &parent, const char *name) {
    for (int row = 0; row < model->rowCount(parent); ++row) {
      const auto idx = model->index(row, 0, parent);
      if (model->data(idx, Qt::DisplayRole).toString() == QLatin1String(name))
        return idx;
    }
    return QModelIndex();
  };

  const auto root_index = model->index(0, 0);
  const auto wrapper    = child_named(root_index, "wrapper");
  const auto wide       = child_named(root_index, "wide");
  REQUIRE(wrapper.isValid());
  REQUIRE(wide.isValid());

  // Expanding has to go through a laid-out view, because that is the only path
  // that reaches the fetch: QTreeView::expand() acts on its own view rows, and
  // with no layout it just records the request. Showing the dialog is also what
  // the user does, so the code under test is the code that runs.
  dlg.show();
  QCoreApplication::processEvents();

  // Walk down one level at a time, asking the two questions in the order a user
  // meets them: is there an arrow, and does opening it do anything. The first
  // question is the regression - the lazy population answered canFetchMore but
  // never hasChildren, so every row below the eagerly fetched root reported
  // itself childless, offered no arrow, and could not be opened at all.
  check(model->hasChildren(wrapper), "a directory holding entries is expandable");
  tree->expand(wrapper);
  QCoreApplication::processEvents();

  const auto textures = child_named(wrapper, "textures");
  const auto hollow   = child_named(wrapper, "hollow");
  const auto readme   = child_named(wrapper, "readme.txt");
  REQUIRE(textures.isValid());
  REQUIRE(hollow.isValid());
  REQUIRE(readme.isValid());

  check(model->hasChildren(textures),
        "a directory at depth 2 is expandable, not a dead end");
  tree->expand(textures);
  QCoreApplication::processEvents();
  check(model->rowCount(textures) == 1, "expanding populates the depth-2 directory");

  const auto deep = child_named(textures, "deep");
  REQUIRE(deep.isValid());
  check(model->hasChildren(deep), "a directory at depth 3 is expandable too");
  tree->expand(deep);
  QCoreApplication::processEvents();
  check(model->rowCount(deep) == 1, "expanding populates the depth-3 directory");
  check(child_named(deep, "gizmo.dds").isValid(),
        "a file four levels down is reachable, so it can be walked to");

  // A leaf is a leaf. "hollow" holds nothing and readme.txt is a file, so
  // neither may offer an arrow that opens onto nothing.
  check(!model->hasChildren(hollow), "an empty directory shows no arrow");
  check(!model->hasChildren(readme), "a file shows no arrow");
  check(!model->hasChildren(child_named(deep, "gizmo.dds")),
        "a leaf file shows no arrow");

  // Laziness is the point of not populating: knowing a directory holds
  // something must not cost reading 500 entries to find out.
  check(model->hasChildren(wide), "a wide directory is still reported as expandable");
  check(model->rowCount(wide) == 0,
        "reporting a wide directory as expandable does not populate it");

  std::error_code ec;
  std::filesystem::remove_all(root, ec);
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

  ui::LayoutDialog dlg(root, "Data", "CoolMod.rar", checker);
  auto *tree  = dlg.findChild<QTreeView *>();
  auto *model = tree ? tree->model() : nullptr;
  check(tree && model, "the dialog has a tree over the extracted content");

  // The row that stands for the extracted content is named after the archive.
  // MO2 labels it "<data>" (archivetree.cpp setup()), which reads as a
  // placeholder and looks like the row already holds game data when it holds the
  // wrapper the peel could not resolve.
  const auto top = model->index(0, 0);
  REQUIRE(top.isValid());
  const auto top_label = model->data(top, Qt::DisplayRole).toString();
  check(top_label == QStringLiteral("CoolMod.rar"),
        "the top row is the archive's name");
  check(!top_label.startsWith(QLatin1Char('<')) &&
            !top_label.endsWith(QLatin1Char('>')),
        "the top row is not an angle-bracket placeholder");

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
  ui::LayoutDialog unchecked(root, "Data", "CoolMod.rar", nullptr);
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
