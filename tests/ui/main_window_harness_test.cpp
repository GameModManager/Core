// Full-window in-process GUI harness (Workspace-qs50).
//
// Drives the REAL ui::MainWindow offscreen (QT_QPA_PLATFORM=offscreen) with
// QTest mouse/hover gestures against the real buttons, tab bar, tables and
// context menus, then asserts QToolTip content, model/view state and
// filesystem side effects. The construction shape is the one already proven
// by game_path_banner_test.cpp / saves_tab_test.cpp: no Core::Application.
//
// Golden paths covered here:
//   1. LOOT sort run + plugin hover -> LOOT bullets in the QToolTip (the
//      regression class that recently bit us). A fake gmm_lootcli script
//      (protocol shared with tests/engine/loot_sorter_test.cpp) is installed
//      beside the test binary at runtime and removed on teardown; the
//      masterlist cache is pre-seeded fresh under the FakePlatform data dir
//      so the controller's default update_masterlists=true is a 24h-TTL
//      cache hit and never touches the network.
//   2. Open the Saves tab (first-show lazy scan of a fixture dir) -> assert
//      rows; switch tabs -> assert last_tab persisted into instance.toml.
//   3. Mod delete via the real context menu + Remove Mods TaskDialog ->
//      folder lands in the (isolated) XDG trash + model row gone.
//
// Explicitly out of scope (documented on Workspace-qs50, not faked):
// real game-process launch (needs a live game install), the real libloot
// gmm_lootcli (network + cargo), and OS-level input injection (QTest
// synthesizes events in-process by design).
//
// Hermeticity: every TEST_CASE uses its own throwaway /tmp/gmm_qs50_* root
// (XDG_CONFIG_HOME + XDG_DATA_HOME point into it), so Settings, the
// freedesktop trash and the masterlist cache are per-test isolated. No
// shared /tmp state between test cases or with other test binaries.
#include "engine/core/instance/instance.h"
#include "engine/game/registry/game_capabilities.h"
#include "engine/game/registry/game_knowledge.h"
#include "platform/platform.h"
#include "ui/controllers/mod_list_controller.h"
#include "ui/main_window/main_window.h"
#include "ui/panels/plugins_tab.h"
#include "ui/panels/saves_tab.h"
#include "ui/settings/settings.h"
#include "ui/widgets/mod_list_model.h"
#include "ui/widgets/mod_table_view.h"
#include "ui/widgets/right_filter_bar.h"
#include "ui/widgets/right_panel.h"

#include <QApplication>
#include <QCommandLinkButton>
#include <QContextMenuEvent>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHelpEvent>
#include <QItemSelectionModel>
#include <QMenu>
#include <QModelIndex>
#include <QPushButton>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QToolTip>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

void write_file(const fs::path &p, const std::string &content) {
  std::ofstream out(p);
  out << content;
}

// Polls pred while pumping the event loop; returns false on timeout so the
// caller FAILs with context instead of hanging the suite.
template <typename Fn>
bool pump_until(Fn pred, int timeout_ms = 15000) {
  QElapsedTimer timer;
  timer.start();
  while (!pred()) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QThread::msleep(2);
    if (timer.elapsed() > timeout_ms)
      return false;
  }
  return true;
}

void pump_ms(int ms) {
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < ms) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QThread::msleep(2);
  }
}
class FakePlatform : public engine::Platform {
public:
  explicit FakePlatform(fs::path data_dir) : data_dir_(std::move(data_dir)) {}
  std::string platform_name() const override { return "fake"; }
  fs::path data_dir() const override { return data_dir_; }
  fs::path config_dir() const override { return data_dir_; }
  fs::path cache_dir() const override { return data_dir_; }
  fs::path home_dir() const override { return data_dir_; }
  fs::path temp_dir() const override { return data_dir_; }
  fs::path find_steam_root() const override { return {}; }
  bool launch_executable(const fs::path &,
                         const std::vector<std::string> &) const override {
    return false;
  }

private:
  fs::path data_dir_;
};

// The fake CLI's identifying marker: a leftover from a previous run is ours
// to replace; a foreign gmm_lootcli (a real GMM_WITH_LOOT=ON build) is not
// touched and the sort case skips instead of clobbering the artifact or
// reaching the network.
constexpr const char *kFakeLootcliMarker = "gmm_qs50 fake lootcli";

std::string fake_lootcli_script() {
  // Same stdout protocol as tests/engine/loot_sorter_test.cpp: progress
  // markers, --pluginListOutputPath / --out parsing, sorted list + MO2
  // lootcli createPlugins-shape JSON report. Sorted output = every seeded
  // plugin (natives first, user band ZetaMod.esp before MyMod.esp) so
  // PluginDb::apply_load_order accepts it and the row swap is observable.
  std::string s;
  s += "#!/bin/sh\n";
  s += "# gmm_qs50 fake lootcli - hermetic harness stand-in, do not run\n";
  s += std::string("echo '") + kFakeLootcliMarker + "'\n";
  s += "echo '[progress] 1'\n";
  s += "echo '[progress] 2'\n";
  s += "echo '[progress] 3'\n";
  s += "echo '[progress] 4'\n";
  s += "echo '[progress] 5'\n";
  s += "echo '[progress] 6'\n";
  s += "out=\"\"\nreport=\"\"\nprev=\"\"\n";
  s += "for arg in \"$@\"; do\n";
  s += "  if [ \"$prev\" = \"--pluginListOutputPath\" ]; then out=\"$arg\"; fi\n";
  s += "  if [ \"$prev\" = \"--out\" ]; then report=\"$arg\"; fi\n";
  s += "  prev=\"$arg\"\n";
  s += "done\n";
  s += "echo '[progress] 7'\n";
  s += "printf 'Skyrim.esm\\n' > \"$out\"\n";
  s += "printf 'Update.esm\\n' >> \"$out\"\n";
  s += "printf 'ZetaMod.esp\\n' >> \"$out\"\n";
  s += "printf 'MyMod.esp\\n' >> \"$out\"\n";
  s += "cat > \"$report\" <<'LOOTJSON'\n";
  s += "{\n";
  s += "  \"game\": \"skyrimse\",\n";
  s += "  \"sortedPlugins\": [\"Skyrim.esm\", \"Update.esm\", "
       "\"ZetaMod.esp\", \"MyMod.esp\"],\n";
  s += "  \"plugins\": [\n";
  s += "    {\"name\": \"ZetaMod.esp\",\n";
  s += "     \"messages\": [{\"type\": \"warn\", \"text\": \"Harness LOOT "
       "warning message\"}],\n";
  s += "     \"dirty\": [{\"crc\": 4242, \"itm\": 7, \"deletedReferences\": 1, "
       "\"deletedNavmesh\": 2,\n";
  s += "                \"cleaningUtility\": \"SSEEdit\", \"info\": \"\"}],\n";
  s += "     \"clean\": [{\"cleaningUtility\": \"SSEEdit\"}],\n";
  s += "     \"missingMasters\": [\"GoneMaster.esm\"]}\n";
  s += "  ]\n";
  s += "}\n";
  s += "LOOTJSON\n";
  s += "exit 0\n";
  return s;
}

// RAII guard: removes the fake CLI we installed, so the build tree is never
// left with a stand-in gmm_lootcli (even when an assertion FAILs).
struct FakeLootcliGuard {
  fs::path path;
  bool owned = false;
  ~FakeLootcliGuard() {
    if (owned) {
      std::error_code ec;
      fs::remove(path, ec);
    }
  }
};

// Installs the fake next to the test binary. Returns false when a foreign
// (real) gmm_lootcli is present - the caller then skips the sort case.
bool install_fake_lootcli(FakeLootcliGuard &guard) {
  const fs::path cli =
      fs::path(QCoreApplication::applicationDirPath().toStdString()) / "gmm_lootcli";
  guard.path = cli;
  std::error_code ec;
  if (fs::is_regular_file(cli, ec)) {
    std::string head;
    {
      std::ifstream raw(cli);
      char buf[512];
      raw.read(buf, sizeof(buf));
      head.assign(buf, static_cast<size_t>(raw.gcount()));
    }
    if (head.find(kFakeLootcliMarker) == std::string::npos)
      return false;  // a real gmm_lootcli - do not clobber, do not network
    fs::remove(cli, ec);
  }
  write_file(cli, fake_lootcli_script());
  fs::permissions(cli, fs::perms::owner_all | fs::perms::group_read |
                           fs::perms::others_read);
  guard.owned = fs::is_regular_file(cli, ec);
  return guard.owned;
}

// QTest-clicks a tab bar header - the real gesture a user performs.
void click_tab(QTabWidget *tabs, const QString &label) {
  REQUIRE(tabs != nullptr);
  auto *bar = tabs->findChild<QTabBar *>();
  REQUIRE(bar != nullptr);
  int idx = -1;
  for (int i = 0; i < bar->count(); ++i) {
    if (bar->tabText(i) == label) {
      idx = i;
      break;
    }
  }
  REQUIRE(idx >= 0);
  QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, bar->tabRect(idx).center());
  QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

int find_row(const QTableWidget *table, const QString &name) {
  for (int r = 0; r < table->rowCount(); ++r) {
    auto *item = table->item(r, 0);
    if (item && item->text() == name)
      return r;
  }
  return -1;
}

// ModList column 0 is the fold indicator, NOT the name - scan every cell of
// every row instead (proxy- and column-order-safe), the same rows the user
// sees in the view.
int find_mod_row(ui::ModView *view, const QString &id) {
  const auto *m = view->model();
  for (int r = 0; r < m->rowCount(); ++r) {
    for (int c = 0; c < m->columnCount(); ++c) {
      if (m->index(r, c).data().toString() == id)
        return r;
    }
  }
  return -1;
}

// Per-case throwaway root: config/data/instances live inside it, so
// Settings (XDG_CONFIG_HOME) and the freedesktop trash (XDG_DATA_HOME) are
// isolated per TEST_CASE and never shared with another test.
fs::path make_case_root(const char *name) {
  const fs::path root = fs::path("/tmp") / name;
  fs::remove_all(root);
  fs::create_directories(root / "config");
  fs::create_directories(root / "data");
  fs::create_directories(root / "instances");
  qputenv("XDG_CONFIG_HOME", QByteArray((root / "config").string().c_str()));
  qputenv("XDG_DATA_HOME", QByteArray((root / "data").string().c_str()));
  // set_game_info posts ensure_nxm_handler_default() via singleShot(0); it
  // pops a modal NXM-handler QMessageBox inside the first processEvents
  // (infinite hang offscreen). The "dont_ask" setting is its early-out -
  // same choice a user makes with "Don't show".
  Settings::instance().set_nxm_handler_check("dont_ask");
  return root;
}

}  // namespace

// ---------------------------------------------------------------------------
// Golden path #1 (priority): LOOT sort run through the real window, then a
// plugin hover must surface the LOOT bullets in QToolTip - the regression
// class that recently bit us (loot reports silently missing from tooltips).
// ---------------------------------------------------------------------------
TEST_CASE("MainWindow: LOOT sort run renders bullets in plugin hover tooltip",
          "[ui][harness]") {
  const fs::path root = make_case_root("gmm_qs50_loot");

  // Skyrim-like game dir: plugins live under <game_dir>/Data (PathResolver).
  const fs::path game_dir = root / "game";
  fs::create_directories(game_dir / "Data");
  write_file(game_dir / "Data" / "Skyrim.esm", "fake esm");
  write_file(game_dir / "Data" / "Update.esm", "fake esm");
  write_file(game_dir / "Data" / "MyMod.esp", "fake esp");
  write_file(game_dir / "Data" / "ZetaMod.esp", "fake esp");

  // Fresh masterlist cache under the FakePlatform data dir: the controller
  // leaves update_masterlists at its default (true), and a fresh cache is a
  // guaranteed no-network hit (masterlists.cpp 24h TTL).
  const fs::path ml_dir = root / "data" / "loot" / "testgame";
  fs::create_directories(ml_dir);
  write_file(ml_dir / "masterlist.yaml", "seq: 1\n");
  write_file(ml_dir / "prelude.yaml", "groups: []\n");

  // QApplication must outlive everything in this case (QToolTip, widgets),
  // so it is declared here - never in a helper that would return first.
  qputenv("QT_QPA_PLATFORM", "offscreen");
  int app_argc     = 1;
  char app_argv0[] = "main_window_harness_test";
  char *app_argv[] = {app_argv0, nullptr};
  QApplication app(app_argc, app_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  FakeLootcliGuard lootcli;
  if (!install_fake_lootcli(lootcli)) {
    WARN("a real gmm_lootcli sits next to the test binary - hermetic fake "
         "not installed; skipping the LOOT sort case");
    fs::remove_all(root);
    return;
  }

  auto inst           = engine::Instance::installed("TestGame", root / "instances");
  inst.info().game_id = "testgame";
  REQUIRE(inst.create_directories());
  REQUIRE(inst.write_toml());

  engine::GameKnowledge knowledge;
  knowledge.set("testgame", "mods_subpath", "Mods");
  knowledge.set("testgame", "game_native_plugins", "Skyrim.esm,Update.esm");
  knowledge.set("testgame", "loot_game_id", "skyrimse");

  FakePlatform platform(root / "data");

  engine::GameCapabilities caps;
  engine::CapabilityInfo plugins_cap;
  plugins_cap.game_id      = "testgame";
  plugins_cap.capability   = "plugins";
  plugins_cap.display_name = "Plugins";
  caps.register_capability(plugins_cap);

  ui::MainWindow w;
  w.set_game_knowledge(&knowledge);
  w.set_platform(&platform);
  // Capabilities must land BEFORE set_game_info: the settings controller
  // only overwrites them when a PluginLoader is wired (it is not here).
  auto *rp = w.findChild<ui::RightPanel *>();
  REQUIRE(rp != nullptr);
  rp->set_capabilities(&caps);
  w.show();
  w.set_game_info("testgame", "Test Game", "Default", game_dir, inst.info().root);

  REQUIRE(pump_until([&w] {
    return !w.is_loading();
  }));

  // Open the Plugins tab the way a user does: QTest-click the tab header.
  // The click materializes the lazy tab, which triggers the production
  // tab_materialized -> refresh_plugins_tab wiring.
  click_tab(rp->tab_widget(), QStringLiteral("Plugins"));
  auto *ptab = rp->plugins_tab();
  REQUIRE(ptab != nullptr);
  auto *table = ptab->table();
  REQUIRE(table != nullptr);
  REQUIRE(pump_until([&] {
    return table->rowCount() == 4;
  }));

  // Pre-sort baseline: natives first, then the user band in on-disk
  // (alphabetical) order - MyMod.esp before ZetaMod.esp.
  CHECK(find_row(table, QStringLiteral("Skyrim.esm")) == 0);
  CHECK(find_row(table, QStringLiteral("Update.esm")) == 1);
  CHECK(find_row(table, QStringLiteral("MyMod.esp")) == 2);
  CHECK(find_row(table, QStringLiteral("ZetaMod.esp")) == 3);

  // The LOOT sort shortcut only shows on the Plugins tab; click it.
  auto *sort_btn = rp->filter_bar()->findChild<QPushButton *>();
  REQUIRE(sort_btn != nullptr);
  REQUIRE(pump_until(
      [&] {
        return sort_btn->isVisible();
      },
      5000));
  QTest::mouseClick(sort_btn, Qt::LeftButton);

  // The sort runs on LootSortThread and lands via on_loot_finished: the
  // user band swaps (ZetaMod.esp first) AND the LOOT report reaches the
  // row tooltip. Both observable through the real table.
  const auto sort_landed = [&] {
    return find_row(table, QStringLiteral("ZetaMod.esp")) == 2 &&
           find_row(table, QStringLiteral("MyMod.esp")) == 3;
  };
  REQUIRE(pump_until(sort_landed, 20000));

  const int zeta_row = find_row(table, QStringLiteral("ZetaMod.esp"));
  REQUIRE(zeta_row >= 0);
  auto *zeta_item = table->item(zeta_row, 0);
  REQUIRE(zeta_item != nullptr);
  const QString row_tip = zeta_item->toolTip();
  CHECK(row_tip.contains("<li>"));
  CHECK(row_tip.contains("GoneMaster.esm"));  // missing-master bullet
  CHECK(row_tip.contains("ITM record"));      // dirty-findings bullet
  CHECK(row_tip.contains("Harness LOOT warning message"));

  // Hover: QTest mouse move + the synthesized QToolTip help event the event
  // loop would deliver after the hover delay. QToolTip::text() must carry
  // the same LOOT bullets - this is the exact assertion the recent loot
  // regressions would have failed.
  auto *viewport = table->viewport();
  REQUIRE(viewport != nullptr);
  const QRect cell = table->visualRect(table->model()->index(zeta_row, 0));
  const QPoint pos = cell.center();
  QTest::mouseMove(viewport, pos);
  QHelpEvent hover(QEvent::ToolTip, pos, viewport->mapToGlobal(pos));
  QCoreApplication::sendEvent(viewport, &hover);
  QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

  const QString tip = QToolTip::text();
  INFO("QToolTip text: " << tip.toStdString());
  CHECK(tip.contains("<li>"));
  CHECK(tip.contains("GoneMaster.esm"));
  CHECK(tip.contains("ITM record"));
  CHECK(tip.contains("Harness LOOT warning message"));
  QToolTip::hideText();

  fs::remove_all(root);
}

// ---------------------------------------------------------------------------
// Golden path: open the Saves tab (real first-show lazy scan of a fixture
// dir) -> assert rows; switch tabs -> assert the last_tab side effect was
// persisted into instance.toml through the production tab_changed wiring.
// ---------------------------------------------------------------------------
TEST_CASE("MainWindow: Saves tab scan rows + tab switch persists last_tab",
          "[ui][harness]") {
  const fs::path root     = make_case_root("gmm_qs50_tabs");
  const fs::path inst_dir = root / "instances";
  const fs::path saves    = root / "saves";
  fs::create_directories(saves);
  write_file(saves / "P_1.ess", "stub save one");
  write_file(saves / "P_2.ess", "stub save two");

  // QApplication must outlive everything in this case (QToolTip, widgets),
  // so it is declared here - never in a helper that would return first.
  qputenv("QT_QPA_PLATFORM", "offscreen");
  int app_argc     = 1;
  char app_argv0[] = "main_window_harness_test";
  char *app_argv[] = {app_argv0, nullptr};
  QApplication app(app_argc, app_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  auto inst           = engine::Instance::installed("TestGame", inst_dir);
  inst.info().game_id = "savesgame";
  REQUIRE(inst.create_directories());
  REQUIRE(inst.write_toml());
  const fs::path inst_root = inst.info().root;

  engine::GameKnowledge knowledge;
  knowledge.set("savesgame", "mods_subpath", "Mods");

  engine::GameCapabilities caps;
  engine::CapabilityInfo saves_cap;
  saves_cap.game_id      = "savesgame";
  saves_cap.capability   = "saves";
  saves_cap.display_name = "Saves";
  caps.register_capability(saves_cap);

  ui::MainWindow w;
  w.set_game_knowledge(&knowledge);
  auto *rp = w.findChild<ui::RightPanel *>();
  REQUIRE(rp != nullptr);
  rp->set_capabilities(&caps);
  w.show();
  w.set_game_info("savesgame", "Save Game", "Default", {}, inst_root);
  REQUIRE(pump_until([&w] {
    return !w.is_loading();
  }));

  // Materialize the tab (wires scan_requested through tab_materialized) and
  // point it at the fixture dir. materialize() itself makes the page current,
  // so its first showEvent fires BEFORE tab_materialized connects the handler
  // (that scan_requested is lost by design - no eager scan at game load).
  auto *st = rp->ensure_saves_tab();
  REQUIRE(st != nullptr);
  st->set_saves_dir(saves);

  // Switch away and back with real clicks: the second show is the first one
  // the wiring observes, so ensure_scanned() emits scan_requested and the
  // lazy first-show scan runs against OUR dir - exactly the production path
  // a user takes when opening the Saves tab.
  click_tab(rp->tab_widget(), QStringLiteral("Data"));
  click_tab(rp->tab_widget(), QStringLiteral("Saves"));
  REQUIRE(pump_until(
      [&] {
        return st->table()->rowCount() == 2;
      },
      20000));
  for (int r = 0; r < st->table()->rowCount(); ++r) {
    REQUIRE(st->table()->item(r, 0) != nullptr);
    CHECK_FALSE(st->table()->item(r, 0)->text().isEmpty());
  }

  // Switch tabs with a real click; the production tab_changed handler
  // persists last_tab into instance.toml (read-before-write).
  click_tab(rp->tab_widget(), QStringLiteral("Data"));
  // data_tab() returns an incomplete type here - assert via the tab label.
  CHECK(rp->tab_widget()->tabText(rp->tab_widget()->currentIndex()) == "Data");

  engine::Instance back = engine::Instance::from_root(inst_root);
  REQUIRE(back.read_toml());
  CHECK(back.info().last_tab == "data");

  fs::remove_all(root);
}

// ---------------------------------------------------------------------------
// Golden path: mod delete through the real context menu + Remove Mods
// TaskDialog -> the folder lands in the (isolated) freedesktop trash with a
// .trashinfo sidecar and the model row is gone.
// ---------------------------------------------------------------------------
TEST_CASE("MainWindow: mod delete moves folder to trash", "[ui][harness]") {
  const fs::path root     = make_case_root("gmm_qs50_delete");
  const fs::path inst_dir = root / "instances";

  // QApplication must outlive everything in this case (QToolTip, widgets),
  // so it is declared here - never in a helper that would return first.
  qputenv("QT_QPA_PLATFORM", "offscreen");
  int app_argc     = 1;
  char app_argv0[] = "main_window_harness_test";
  char *app_argv[] = {app_argv0, nullptr};
  QApplication app(app_argc, app_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  auto inst           = engine::Instance::installed("TestGame", inst_dir);
  inst.info().game_id = "testgame";
  REQUIRE(inst.create_directories());
  REQUIRE(inst.write_toml());
  const fs::path inst_root = inst.info().root;
  const fs::path mods_dir =
      engine::Instance::from_root(inst_root).path_for(engine::InstanceKind::Mods);
  fs::create_directories(mods_dir / "Foo_mod");
  write_file(mods_dir / "Foo_mod" / "meta.ini", "[General]\npriority=0\n");

  engine::GameKnowledge knowledge;
  knowledge.set("testgame", "mods_subpath", "Mods");

  ui::MainWindow w;
  w.set_game_knowledge(&knowledge);
  w.show();
  // Game-less instance (banner-test shape): instance-owned mod ops work
  // without a game dir.
  w.set_game_info("testgame", "Test Game", "Default", {}, inst_root);
  REQUIRE(pump_until([&w] {
    return !w.is_loading();
  }));

  auto *model = w.findChild<ui::ModList *>();
  REQUIRE(model != nullptr);
  auto *view = w.mod_view();
  REQUIRE(view != nullptr);
  REQUIRE(pump_until([&] {
    return find_mod_row(view, QStringLiteral("Foo_mod")) >= 0;
  }));

  const int row = find_mod_row(view, QStringLiteral("Foo_mod"));
  REQUIRE(row >= 0);

  // Safety watchdog: if the scripted menu/dialog dance never runs, close
  // whatever is open after 10s so the case FAILs instead of hanging.
  QTimer::singleShot(10000, [] {
    if (auto *popup = QApplication::activePopupWidget())
      popup->close();
    if (auto *modal = QApplication::activeModalWidget())
      modal->close();
  });

  // Select the row with a real left click (SelectRows behavior).
  view->scrollTo(view->model()->index(row, 0));
  QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  const QRect cell = view->visualRect(view->model()->index(row, 0));
  REQUIRE(cell.isValid());
  QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, cell.center());
  REQUIRE(view->selectionModel()->selectedRows().size() == 1);

  // Scripted interaction, armed BEFORE the context menu opens (the timers
  // fire inside QMenu::exec's nested loop - the task_dialog_test pattern):
  //   1. trigger the real "Remove" action, whose lambda opens the
  //      Remove Mods TaskDialog synchronously;
  //   2. while THAT dialog's exec loop spins, click its "Yes" command link.
  bool menu_seen   = false;
  bool dialog_seen = false;
  QTimer::singleShot(0, [&] {
    auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
    if (!menu)
      return;
    menu_seen       = true;
    QAction *remove = nullptr;
    for (auto *a : menu->actions()) {
      if (a->text() == QStringLiteral("Remove")) {
        remove = a;
        break;
      }
    }
    if (!remove) {
      menu->close();
      return;
    }
    QTimer::singleShot(0, [&] {
      // ui::TaskDialog has no Q_OBJECT, so no qobject_cast to it - the
      // modal is a plain QWidget and is identified by its Yes command link.
      auto *dlg = QApplication::activeModalWidget();
      if (!dlg)
        return;
      dialog_seen = true;
      for (auto *b : dlg->findChildren<QCommandLinkButton *>()) {
        if (b->text() == QStringLiteral("Yes")) {
          b->click();
          return;
        }
      }
      dlg->close();  // unexpected button layout: unblock, let assertions fail
    });
    remove->trigger();
  });

  // CustomContextMenu policy: deliver the context-menu event directly -
  // QTest synthesizes mouse events without the window system's context
  // menu generation.
  QContextMenuEvent ctx(QContextMenuEvent::Mouse, cell.center(),
                        view->viewport()->mapToGlobal(cell.center()));
  QCoreApplication::sendEvent(view->viewport(), &ctx);

  REQUIRE(menu_seen);
  REQUIRE(dialog_seen);

  // Side effects: model row gone, folder in the isolated trash + sidecar.
  CHECK(find_mod_row(view, QStringLiteral("Foo_mod")) < 0);
  CHECK_FALSE(fs::exists(mods_dir / "Foo_mod"));
  const fs::path trash_root =
      fs::path(qgetenv("XDG_DATA_HOME").toStdString()) / "Trash";
  CHECK(fs::exists(trash_root / "files" / "Foo_mod"));
  CHECK(fs::exists(trash_root / "info" / "Foo_mod.trashinfo"));

  fs::remove_all(root);
}
