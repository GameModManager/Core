// Menu shape: the Help tree and the keyboard table.
//
// Help menu - the order MO2 builds in MainWindow::createHelpMenu()
// (references/modorganizer/src/mainwindow.cpp:1080-1163) is Help on UI,
// Documentation, Game Support Wiki (only when the managed game carries a
// support URL), Chat on Discord, Report Issue, Tutorials, About, About Qt -
// one level deep throughout, with Tutorials the only submenu. GMM folds
// Documentation, Report Issue, About and About Qt under a More submenu, which
// MO2 has no equivalent of: this is a deliberate difference, not parity. The
// Tutorials entries come from tutorials/*.js "//TL" headers sorted by their
// order value (First Steps / Conflict Resolution / Overview); nothing ships
// them yet, so the submenu stands empty, as MO2's does when its directory is
// empty. GMM has no Discord presence, so that entry is dropped rather than
// left disabled, and the two link entries point at this repo instead of
// MO2's.
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

// What the user actually sees at one level: entries in order, separators and
// hidden entries dropped. A submenu shows up as its own title, so the grouping
// is not flattened away - the More children are asserted against More.
std::vector<std::string> menu_entries(const QMenu *menu) {
  std::vector<std::string> out;
  for (const QAction *act : menu->actions()) {
    if (act->isSeparator() || !act->isVisible())
      continue;
    out.push_back(act->text().toStdString());
  }
  return out;
}

// The Help menu bar's own entries, in order. Game Support Wiki is not here: it
// is gated on a game support URL and none exists, so it is asserted on its own
// below rather than padding the list with an entry nobody can see.
const std::vector<std::string> kHelpTopLevel = {
    "Help on UI", "More", "Tutorials", "Instance Statistics...", "Debug Panel"};

// What More folds one level down, in order. No separators inside it.
const std::vector<std::string> kMoreOrder = {
    "Documentation", "Report Issue", "About GameModManager", "About Qt"};

// The project's own addresses. The link entries carry the destination in the
// QAction's data() so the destination is readable without firing the action.
const std::string kRepoUrl  = "https://github.com/GameModManager/Core";
const std::string kIssueUrl = "https://github.com/GameModManager/Core/issues";

// Recursive on purpose: four of the Help entries live under More now, so a
// flat lookup would miss them and a flattened list would hide the grouping.
QAction *find_action(const QMenu *menu, const std::string &text) {
  for (QAction *act : menu->actions()) {
    if (act->text().toStdString() == text)
      return act;
    if (QMenu *sub = act->menu()) {
      if (QAction *hit = find_action(sub, text))
        return hit;
    }
  }
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

TEST_CASE("Help menu carries its own tree", "[ui][menu][parity]") {
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

  SECTION("the menu bar's own entries appear in the requested order") {
    CHECK(menu_entries(help) == kHelpTopLevel);
  }

  SECTION("More folds the four entries one level down, in order") {
    auto *more = find_action(help, "More");
    REQUIRE(more != nullptr);
    REQUIRE(more->menu() != nullptr);
    CHECK(menu_entries(more->menu()) == kMoreOrder);
    // Nothing folded twice, and no separator smuggled inside.
    for (const QAction *act : more->menu()->actions())
      CHECK(act->menu() == nullptr);
  }

  SECTION("the separator splits the help block from the GMM-only entries") {
    // Help on UI / More / Tutorials above it, Instance Statistics and Debug
    // Panel below: the split position, not just the presence of a separator.
    const auto actions = help->actions();
    const auto sep =
        std::find_if(actions.begin(), actions.end(),
                     [](const QAction *act) { return act->isSeparator(); });
    REQUIRE(sep != actions.end());
    const auto above = std::count_if(actions.begin(), sep, [](const QAction *act) {
      return !act->isSeparator() && act->isVisible();
    });
    CHECK(above == 3);
    CHECK(std::count_if(sep, actions.end(), [](const QAction *act) {
            return !act->isSeparator() && act->isVisible();
          }) == 2);
  }

  SECTION("Help on UI is a real entry, not a stub") {
    auto *act = find_action(help, "Help on UI");
    REQUIRE(act != nullptr);
    CHECK(act->menu() == nullptr);  // stays on the menu bar itself
    CHECK(act->isEnabled());
  }

  SECTION("Tutorials is a submenu that stands empty, as MO2's does") {
    auto *act = find_action(help, "Tutorials");
    REQUIRE(act != nullptr);
    REQUIRE(act->menu() != nullptr);
    // MO2 adds the submenu and never disables it
    // (mainwindow.cpp:1119-1160); an empty one is how it says nothing ships.
    CHECK(act->isEnabled());
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
    // are live, so both entries are enabled. Folding them under More moved
    // them, it did not rewire them.
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
