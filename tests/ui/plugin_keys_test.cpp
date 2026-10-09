// Offscreen GUI test for the plugin list's keyboard routing (MO2
// PluginListView::event, pluginlistview.cpp:382-410).
//
// What is pinned:
//   - Space flips the enable state of every selected row and emits one
//     toggle_requested per row.
//   - Ctrl+Up / Ctrl+Down shift the selection one place, but ONLY while the
//     table is sorted by the Priority or Mod Index column. MO2 gates on the
//     sort column for exactly this reason (pluginlistview.cpp:401-403);
//     without the gate the keys would fight the plain arrow keys.
//   - Ctrl+Return reveals the owning mod of a single selected row, and does
//     nothing on a game-Data row or a multi-selection.
//
// The shift arithmetic itself - the band clamp, the pinned rows, the
// direction the selection is walked in, and the priority renumbering - is
// engine code and lives in plugin_database_test, which drives the real
// Database on a real fixture. This file pins the routing only.
//
// Hermetic: offscreen platform, throwaway XDG_CONFIG_HOME, no network.
#include "ui/panels/tab_panels.h"
#include "ui/settings/settings.h"
#include "ui/theme/icon_manager.h"

#include <QApplication>
#include <QCoreApplication>
#include <QHeaderView>
#include <QKeyEvent>
#include <QTableWidget>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace {
void check(bool cond, const char *what) {
  INFO(what);
  REQUIRE(cond);
}

int row_with_name(QTableWidget *t, const char *name) {
  for (int r = 0; r < t->rowCount(); ++r) {
    auto *item = t->item(r, 0);
    if (item && item->text() == QLatin1String(name))
      return r;
  }
  return -1;
}

// Delivered straight to the table, which is where a real key press lands once
// the table has focus. The routing under test is the table's own
// keyPressEvent, not Qt's focus machinery.
void press_key(QWidget *w, int k, Qt::KeyboardModifiers mods = Qt::NoModifier) {
  QKeyEvent press(QEvent::KeyPress, k, mods);
  QApplication::sendEvent(w, &press);
  QApplication::processEvents();
}

// Two game-native rows (the fixed band) then three user plugins, so the row
// indices double as load-order priorities.
std::vector<engine::GamePlugin> make_plugins() {
  std::vector<engine::GamePlugin> v;
  for (const char *n : {"Skyrim.esm", "Update.esm"}) {
    engine::GamePlugin g;
    g.name              = n;
    g.has_master_ext    = true;
    g.is_master_flagged = true;
    g.is_game_native    = true;
    g.force_loaded      = true;
    g.enabled           = true;
    v.push_back(g);
  }
  int user = 0;
  for (const char *n : {"Alpha.esp", "Beta.esp", "Gamma.esp"}) {
    engine::GamePlugin m;
    m.name    = n;
    m.enabled = true;
    // Each user plugin comes from its own mod, so "reveal the owner" has
    // something to report. The game-native rows above deliberately have no
    // owner_mod, which is the "nothing to open" case MO2 returns false for.
    m.owner_mod = std::string("Mod") + char('A' + user++);
    v.push_back(m);
  }
  for (size_t i = 0; i < v.size(); ++i)
    v[i].priority = static_cast<int>(i);
  return v;
}
}  // namespace

struct TestPluginsTab : ui::PluginsTab {
  using ui::PluginsTab::add_context_menu_actions;
};

TEST_CASE("plugin list keyboard routing", "[ui][keys]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path cfg = "/tmp/gmm_plugin_keys/config";
  std::filesystem::remove_all("/tmp/gmm_plugin_keys");
  std::filesystem::create_directories(cfg);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  int test_argc     = 1;
  char test_argv0[] = "test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");
  engine::IconManager::instance().discover_packs(GMM_TEST_RESOURCES_DIR);

  TestPluginsTab tab;
  auto *table = tab.table();
  tab.resize(600, 300);
  tab.show();
  tab.set_plugins(make_plugins());
  QApplication::processEvents();

  const int alpha = row_with_name(table, "Alpha.esp");
  const int beta  = row_with_name(table, "Beta.esp");
  const int game  = row_with_name(table, "Skyrim.esm");
  check(alpha == 2 && beta == 3 && game == 0, "rows in load order");

  // Collected by hand rather than with QSignalSpy: the signal carries a
  // std::string and a std::vector<int>, which QSignalSpy cannot store.
  std::vector<std::pair<std::string, bool>> toggles;
  QObject::connect(&tab, &ui::PluginsTab::toggle_requested,
                   [&](const std::string &name, bool enabled) {
                     toggles.emplace_back(name, enabled);
                   });
  std::vector<std::pair<std::vector<int>, int>> shifts;
  QObject::connect(&tab, &ui::PluginsTab::shift_requested,
                   [&](const std::vector<int> &rows, int offset) {
                     shifts.emplace_back(rows, offset);
                   });
  std::vector<std::string> reveals;
  QObject::connect(&tab, &ui::PluginsTab::reveal_requested,
                   [&](const std::string &owner) {
                     reveals.push_back(owner);
                   });

  auto select_two = [table, alpha, beta] {
    table->clearSelection();
    table->selectionModel()->select(table->model()->index(alpha, 0),
                                    QItemSelectionModel::ClearAndSelect |
                                        QItemSelectionModel::Rows);
    table->selectionModel()->select(table->model()->index(beta, 0),
                                    QItemSelectionModel::Select |
                                        QItemSelectionModel::Rows);
    QApplication::processEvents();
  };

  SECTION("Space flips every selected row") {
    table->selectRow(alpha);
    select_two();
    REQUIRE(table->selectionModel()->selectedRows().size() == 2);
    REQUIRE(table->item(alpha, 0)->checkState() == Qt::Checked);
    press_key(table, Qt::Key_Space);
    QApplication::processEvents();
    check(toggles.size() == 2, "one toggle per selected row");
    // Both rows start enabled, so Space disables both.
    check(toggles[0].first == "Alpha.esp" && !toggles[0].second,
          "Alpha reported as disabled");
    check(toggles[1].first == "Beta.esp" && !toggles[1].second,
          "Beta reported as disabled");
  }

  SECTION("Space with nothing selected does nothing") {
    table->clearSelection();
    press_key(table, Qt::Key_Space);
    check(toggles.empty(), "an empty selection toggles nothing");
  }

  SECTION("Ctrl+Up/Down shift the selected rows one place") {
    table->selectRow(beta);
    QApplication::processEvents();

    press_key(table, Qt::Key_Down, Qt::ControlModifier);
    REQUIRE(shifts.size() == 1);
    check(shifts[0].first == std::vector<int>{beta},
          "Ctrl+Down reports the selected row");
    check(shifts[0].second == 1, "Ctrl+Down asks for offset +1");

    shifts.clear();
    press_key(table, Qt::Key_Up, Qt::ControlModifier);
    REQUIRE(shifts.size() == 1);
    check(shifts[0].second == -1, "Ctrl+Up asks for offset -1");

    // A multi-row selection reports every selected row, so the engine can
    // move them as a block instead of collapsing them onto each other.
    shifts.clear();
    select_two();
    press_key(table, Qt::Key_Down, Qt::ControlModifier);
    REQUIRE(shifts.size() == 1);
    check(shifts[0].first == std::vector<int>{alpha, beta},
          "both selected rows are reported");
  }

  SECTION("Ctrl+Up/Down with nothing selected does nothing") {
    table->clearSelection();
    QApplication::processEvents();
    press_key(table, Qt::Key_Down, Qt::ControlModifier);
    check(shifts.empty(), "an empty selection shifts nothing");
  }

  SECTION("Ctrl+Return reveals the owner of a single selected row only") {
    table->selectRow(alpha);
    QApplication::processEvents();
    press_key(table, Qt::Key_Return, Qt::ControlModifier);
    check(reveals.size() == 1, "a single selection reveals once");
    check(reveals[0] == "ModA", "the owning mod is named");

    reveals.clear();
    table->selectRow(game);
    QApplication::processEvents();
    press_key(table, Qt::Key_Return, Qt::ControlModifier);
    check(reveals.empty(), "a game-Data plugin owns no mod, so nothing is revealed");

    reveals.clear();
    select_two();
    press_key(table, Qt::Key_Return, Qt::ControlModifier);
    check(reveals.empty(), "a multi-selection reveals nothing");
  }

  SECTION("plain Up/Down stay the arrow keys") {
    table->selectRow(alpha);
    QApplication::processEvents();
    press_key(table, Qt::Key_Down);
    press_key(table, Qt::Key_Up);
    check(shifts.empty(), "an unmodified arrow key never shifts the load order");
    check(toggles.empty(), "an unmodified arrow key never toggles");
  }

  tab.hide();
}
