// MO2 menu parity: the Help tree and the keyboard table.
//
// Help menu - MO2 builds it in MainWindow::createHelpMenu()
// (references/modorganizer/src/mainwindow.cpp:1080-1163) in this exact order:
// Help on UI, Documentation, Game Support Wiki (only when the managed game
// carries a support URL), Chat on Discord, Report Issue, Tutorials, About,
// About Qt. The Tutorials entries come from tutorials/*.js "//TL" headers
// sorted by their order value (First Steps / Conflict Resolution / Overview).
// GMM has no Discord presence, so that one entry is dropped rather than left
// disabled, and the two link entries point at this repo instead of MO2's.
//
// Shortcuts - the eight MO2 binds, read straight out of mainwindow.ui:
//   Ctrl+M Install Mod   Ctrl+P Profiles    Ctrl+E Executables  Ctrl+I Tool Plugins
//   Ctrl+S Settings      Ctrl+N Visit Nexus Ctrl+H Help menu     F5   Refresh
//
// Asserted against the real AppMenuBar, built standalone: the menu bar owns
// every action and its shortcut, so the menu bar IS the thing under test. No
// MainWindow, no game, no instance, no files touched.
//
// All eight of MO2's keys are adopted. Three GMM entries (New Instance, Enable
// Selected, Workflow Pipeline) gave up the key that stood where MO2 puts
// Profiles / the mod source page / Executables; they keep their menu route and
// a test asserts both halves of that, so a silent re-bind cannot creep back in.
// A tree-wide uniqueness check pins the rule that got broken twice already:
// one key, one action.
#include "ui/widgets/menu_bar.h"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace {

// The Help menu as MO2 builds it, minus the support-URL entry (conditional, so
// it is asserted on its own) and the GMM-only entries appended below it. About
// keeps GMM's longer "About GameModManager" label - it is the same entry.
// Chat on Discord is gone: GMM has no Discord presence to point at.
const std::vector<std::string> kMo2HelpOrder = {
    "Help on UI", "Documentation",   "Report Issue", "Tutorials",
    "About GameModManager", "About Qt"};

// The project's own addresses. The Help menu's link entries carry the
// destination in the QAction's data() so the destination is readable without
// firing the action.
const std::string kRepoUrl  = "https://github.com/GameModManager/Core";
const std::string kIssueUrl = "https://github.com/GameModManager/Core/issues";

// What the user actually sees: entries in order, separators and hidden
// entries dropped.
std::vector<std::string> menu_entries(const QMenu *menu) {
  std::vector<std::string> out;
  for (const QAction *act : menu->actions()) {
    if (act->isSeparator() || !act->isVisible())
      continue;
    out.push_back(act->text().toStdString());
  }
  return out;
}

QAction *find_action(const QMenu *menu, const std::string &text) {
  for (QAction *act : menu->actions())
    if (act->text().toStdString() == text)
      return act;
  return nullptr;
}

// Every action reachable from the menu bar, menus walked recursively, so a
// shortcut hiding on a submenu (Icons, Checkerboard, Tutorials) is still
// found. Separators and the menu actions themselves carry no shortcut.
void walk(const QMenu *menu, std::vector<const QAction *> &out) {
  for (QAction *act : menu->actions()) {
    if (act->isSeparator())
      continue;
    if (QMenu *sub = act->menu()) {
      walk(sub, out);
      continue;
    }
    out.push_back(act);
  }
}

// key -> action text, for every bound action in the whole menu bar.
std::map<std::string, std::string> shortcut_table(const ui::AppMenuBar &bar) {
  std::vector<const QAction *> acts;
  for (QAction *top : bar.actions()) {
    if (QMenu *menu = top->menu())
      walk(menu, acts);
  }
  std::map<std::string, std::string> table;
  for (const QAction *act : acts) {
    for (const QKeySequence &seq : act->shortcuts()) {
      if (!seq.isEmpty())
        table[seq.toString().toStdString()] = act->text().toStdString();
    }
  }
  return table;
}

QMenu *menu_named(const ui::AppMenuBar &bar, const std::string &title) {
  for (QAction *act : bar.actions()) {
    QMenu *menu = act->menu();
    if (menu && menu->title().toStdString() == title)
      return menu;
  }
  return nullptr;
}

QKeySequence ctrl(int key) {
  return QKeySequence(Qt::CTRL | static_cast<Qt::Key>(key));
}

}  // namespace

TEST_CASE("Help menu mirrors MO2's tree", "[ui][menu][parity]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path root = "/tmp/opencode/gmm_menu_parity/config";
  std::filesystem::remove_all("/tmp/opencode/gmm_menu_parity");
  std::filesystem::create_directories(root);
  qputenv("XDG_CONFIG_HOME", root.c_str());

  int test_argc     = 1;
  char test_argv0[] = "test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  ui::AppMenuBar bar(nullptr);
  auto *help = menu_named(bar, "&Help");
  REQUIRE(help != nullptr);

  SECTION("MO2's entries appear in MO2's order, unseparated") {
    const auto entries = menu_entries(help);
    std::vector<std::string> without_gmm_extras;
    for (const auto &text : entries) {
      if (text == "Instance Statistics..." || text == "Debug Panel")
        continue;  // GMM-only, appended after MO2's block
      without_gmm_extras.push_back(text);
    }
    CHECK(without_gmm_extras == kMo2HelpOrder);
  }

  SECTION("the GMM-only entries survive, below MO2's block") {
    const auto entries = menu_entries(help);
    const auto mo2_end = std::find(entries.begin(), entries.end(), "About Qt");
    REQUIRE(mo2_end != entries.end());
    CHECK(std::find(entries.begin(), entries.end(), "Instance Statistics...") >
          mo2_end);
    CHECK(std::find(entries.begin(), entries.end(), "Debug Panel") > mo2_end);
  }

  SECTION("Help on UI is a real entry, not a stub") {
    auto *act = find_action(help, "Help on UI");
    REQUIRE(act != nullptr);
    CHECK(act->isEnabled());
  }

  SECTION("Tutorials is stubbed disabled: no tutorial content ships yet") {
    auto *act = find_action(help, "Tutorials");
    REQUIRE(act != nullptr);
    REQUIRE(act->menu() != nullptr);
    CHECK_FALSE(act->isEnabled());
    CHECK(act->menu()->actions().empty());
  }

  SECTION("Game Support Wiki is hidden until the game carries a support URL") {
    auto *act = find_action(help, "Game Support Wiki");
    REQUIRE(act != nullptr);
    // No game in the registry carries one, so the entry must not be offered.
    CHECK_FALSE(act->isVisible());
    auto visible = menu_entries(help);
    CHECK(find(visible.begin(), visible.end(), "Game Support Wiki") == visible.end());

    bar.set_game_support_url("https://example.invalid/support");
    CHECK(act->isVisible());
    CHECK(act->isEnabled());
    visible = menu_entries(help);
    CHECK(find(visible.begin(), visible.end(), "Game Support Wiki") != visible.end());

    bar.set_game_support_url("");
    CHECK_FALSE(act->isVisible());
    visible = menu_entries(help);
    CHECK(find(visible.begin(), visible.end(), "Game Support Wiki") == visible.end());
  }

  SECTION("the project link entries point at the Core repo") {
    // MO2 hardcodes its own destinations (mainwindow.cpp:2352-2370). GMM points
    // Documentation at the Core repo and issue reporting at its tracker; both
    // are live, so both entries are enabled.
    auto *doc = find_action(help, "Documentation");
    REQUIRE(doc != nullptr);
    CHECK(doc->isEnabled());
    CHECK(doc->data().toString().toStdString() == kRepoUrl);

    auto *issue = find_action(help, "Report Issue");
    REQUIRE(issue != nullptr);
    CHECK(issue->isEnabled());
    CHECK(issue->data().toString().toStdString() == kIssueUrl);
  }

  SECTION("no Discord entry: GMM has no Discord presence to link to") {
    CHECK(find_action(help, "Chat on Discord") == nullptr);
    const auto entries = menu_entries(help);
    CHECK(find(entries.begin(), entries.end(), "Chat on Discord") == entries.end());
  }

  std::filesystem::remove_all("/tmp/opencode/gmm_menu_parity");
}

TEST_CASE("menu shortcuts match MO2's table", "[ui][menu][parity][shortcuts]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path root = "/tmp/opencode/gmm_menu_parity/config";
  std::filesystem::remove_all("/tmp/opencode/gmm_menu_parity");
  std::filesystem::create_directories(root);
  qputenv("XDG_CONFIG_HOME", root.c_str());

  int test_argc     = 1;
  char test_argv0[] = "test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  ui::AppMenuBar bar(nullptr);
  auto table = shortcut_table(bar);

  SECTION("MO2's keys that GMM can adopt without displacing anything") {
    // Ctrl+I: MO2's Tool Plugins, the plugin list.
    CHECK(table["Ctrl+I"] == "Tool Plugins");
    // Ctrl+H: MO2 binds it on the Help menu action itself. QMenu has no
    // setShortcut in Qt 6, so the menu's menuAction() carries it.
    CHECK(menu_named(bar, "&Help")->menuAction()->shortcut() == ctrl(Qt::Key_H));
    // Ctrl+S: MO2's Settings.
    CHECK(table["Ctrl+S"] == "Settings...");
    // F5: MO2's Refresh, already correct.
    CHECK(table[QKeySequence(QKeySequence::Refresh).toString().toStdString()] ==
          "Refresh");
  }

  SECTION("adopting a MO2 key does not cost the binding it replaces") {
    // Ctrl+M is MO2's Install Mod, so it lands on Import Mods. Ctrl+Shift+I
    // was the entry's own key and stays: both resolve to the same action.
    CHECK(table["Ctrl+M"] == "Import Mods...");
    auto *import = find_action(menu_named(bar, "&File"), "Import Mods...");
    REQUIRE(import != nullptr);
    CHECK(import->shortcuts().contains(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_I)));

    // Ctrl+Shift+M and Ctrl+Shift+E sat next to the MO2 keys and must not have
    // been dragged onto them: Export Modpack keeps Ctrl+Shift+M, Export Mods
    // keeps Ctrl+Shift+E.
    CHECK(table["Ctrl+Shift+M"] == "Export Modpack...");
    CHECK(table["Ctrl+Shift+E"] == "Export Mods...");

    auto *settings = find_action(menu_named(bar, "&File"), "Settings...");
    REQUIRE(settings != nullptr);
    CHECK(settings->shortcuts().contains(QKeySequence::Preferences));
  }

  SECTION("MO2's four keys reach the MO2 action") {
    // Ctrl+P Profiles / Ctrl+N the mod's source page / Ctrl+E Executables /
    // Ctrl+M install from an archive.
    CHECK(table["Ctrl+P"] == "Profiles...");
    CHECK(table["Ctrl+N"] == "Open Mod Source Page");
    CHECK(table["Ctrl+E"] == "Executables...");
    CHECK(table["Ctrl+M"] == "Import Mods...");
  }

  SECTION("the MO2 entries sit where MO2 keeps them") {
    // mainwindow.ui: File = Change Game, Install Mod, Nexus, ---, Exit;
    // Tools = Add Profile, Modify Executables, ---, Tool, ---, Settings.
    auto *file  = menu_named(bar, "&File");
    auto *tools = menu_named(bar, "&Tools");
    REQUIRE(file != nullptr);
    REQUIRE(tools != nullptr);
    CHECK(find_action(file, "Open Mod Source Page") != nullptr);
    CHECK(find_action(tools, "Profiles...") != nullptr);
    CHECK(find_action(tools, "Executables...") != nullptr);
  }

  SECTION("the entries that gave up their key are still reachable from the menu") {
    // Adopting MO2's keys cost three entries their shortcut, not their
    // route: each stays a first-class item in its own menu.
    CHECK(find_action(menu_named(bar, "&File"), "New Instance...") != nullptr);
    CHECK(find_action(menu_named(bar, "&Edit"), "Enable Selected") != nullptr);
    CHECK(find_action(menu_named(bar, "&View"), "Workflow Pipeline...") != nullptr);

    // And none of them kept the key they gave up, so the four above are the
    // only owners of Ctrl+P / Ctrl+N / Ctrl+E.
    auto *new_instance = find_action(menu_named(bar, "&File"), "New Instance...");
    CHECK(new_instance->shortcut().isEmpty());
    auto *enable = find_action(menu_named(bar, "&Edit"), "Enable Selected");
    CHECK(enable->shortcut().isEmpty());
    auto *pipeline = find_action(menu_named(bar, "&View"), "Workflow Pipeline...");
    CHECK(pipeline->shortcut().isEmpty());
  }

  SECTION("no two actions in the whole menu bar claim the same shortcut") {
    // General guard over the entire tree, submenus included: a key belongs to
    // exactly one action. shortcut_table() cannot catch this (a repeated key
    // just overwrites), so the pairs are collected separately.
    std::vector<const QAction *> acts;
    for (QAction *top : bar.actions()) {
      // The menu's own action carries the key that pops the menu - Ctrl+H on
      // Help - and walk() descends into a menu action rather than collecting
      // it, so it is collected here or the guard would miss it.
      acts.push_back(top);
      if (QMenu *menu = top->menu())
        walk(menu, acts);
    }
    std::map<std::string, std::vector<std::string>> owners;
    for (const QAction *act : acts) {
      for (const QKeySequence &seq : act->shortcuts()) {
        if (seq.isEmpty())
          continue;
        const auto key = seq.toString().toStdString();
        auto &list     = owners[key];
        if (std::find(list.begin(), list.end(), act->text().toStdString()) ==
            list.end())
          list.push_back(act->text().toStdString());
      }
    }
    for (const auto &[key, texts] : owners) {
      INFO("key: " << key);
      CHECK(texts.size() == 1);
    }
  }

  SECTION("the filter-bar pair is not shadowed by any menu action") {
    // Ctrl+F and Escape are window-scoped QShortcuts (MainWindow), not menu
    // actions; MO2's Help menu owns neither, so nothing may claim them here.
    CHECK(table.find("Ctrl+F") == table.end());
    CHECK(table.find("Esc") == table.end());
  }

  std::filesystem::remove_all("/tmp/opencode/gmm_menu_parity");
}
