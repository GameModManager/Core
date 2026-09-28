// MO2 menu parity: the Help tree and the keyboard table.
//
// Help menu - MO2 builds it in MainWindow::createHelpMenu()
// (references/modorganizer/src/mainwindow.cpp:1080-1163) in this exact order:
// Help on UI, Documentation, Game Support Wiki (only when the managed game
// carries a support URL), Chat on Discord, Report Issue, Tutorials, About,
// About Qt. The Tutorials entries come from tutorials/*.js "//TL" headers
// sorted by their order value (First Steps / Conflict Resolution / Overview).
//
// Shortcuts - the eight MO2 binds, read straight out of mainwindow.ui:
//   Ctrl+M Install Mod   Ctrl+P Profiles    Ctrl+E Executables  Ctrl+I Tool Plugins
//   Ctrl+S Settings      Ctrl+N Visit Nexus Ctrl+H Help menu     F5   Refresh
//
// Asserted against the real AppMenuBar, built standalone: the menu bar owns
// every action and its shortcut, so the menu bar IS the thing under test. No
// MainWindow, no game, no instance, no files touched.
//
// The two keys GMM cannot adopt without taking a key away from an existing
// entry (Ctrl+P Workflow Pipeline, Ctrl+N New Instance vs MO2's Profiles and
// Visit Nexus) are pinned by an explicit assertion, so the conflict is visible
// in the suite rather than only in a commit message: a future rebind has to
// update this test deliberately.
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
const std::vector<std::string> kMo2HelpOrder = {
    "Help on UI", "Documentation",        "Chat on Discord", "Report Issue",
    "Tutorials",  "About GameModManager", "About Qt"};

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

  SECTION("the URL entries are listed but disabled while no URL is configured") {
    // MO2 hardcodes its own destinations (mainwindow.cpp:2352-2370). GMM has
    // no project URL configured, so the entries are offered disabled rather
    // than pointing at a made-up or borrowed address.
    for (const char *title : {"Documentation", "Chat on Discord", "Report Issue"}) {
      auto *act = find_action(help, title);
      INFO("entry: " << title);
      REQUIRE(act != nullptr);
      CHECK_FALSE(act->isEnabled());
    }
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
    // Ctrl+M: MO2's Install Mod. GMM's 'install a new mod from an archive' is
    // File > Import Mods...
    CHECK(table["Ctrl+M"] == "Import Mods...");
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
    auto *import = find_action(menu_named(bar, "&File"), "Import Mods...");
    REQUIRE(import != nullptr);
    CHECK(import->shortcuts().contains(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_I)));

    auto *settings = find_action(menu_named(bar, "&File"), "Settings...");
    REQUIRE(settings != nullptr);
    CHECK(settings->shortcuts().contains(QKeySequence::Preferences));
  }

  SECTION("keys still on their pre-parity GMM action, pending the user's call") {
    // MO2 wants Ctrl+P Profiles and Ctrl+N Visit Nexus. Both keys are taken
    // here, so neither was stolen: the pipeline and the instance switcher
    // keep working and MO2's Profiles / Visit Nexus have no menu entry yet.
    CHECK(table["Ctrl+P"] == "Workflow Pipeline...");
    CHECK(table["Ctrl+N"] == "New Instance...");
  }

  SECTION("the filter-bar pair is not shadowed by any menu action") {
    // Ctrl+F and Escape are window-scoped QShortcuts (MainWindow), not menu
    // actions; MO2's Help menu owns neither, so nothing may claim them here.
    CHECK(table.find("Ctrl+F") == table.end());
    CHECK(table.find("Esc") == table.end());
  }

  std::filesystem::remove_all("/tmp/opencode/gmm_menu_parity");
}
