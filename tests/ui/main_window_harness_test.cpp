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
//   1b. LOOT sort UI lock follows Settings::lock_gui - both gate states in
//      one case, since an on-case-only check stays green with the gate gone.
//      Same fake CLI, held for 2s so the window state is read while the
//      subprocess is still running.
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
#include "engine/instance/instance.h"
#include "engine/game/registry/game_capabilities.h"
#include "engine/game/registry/game_knowledge.h"
#include "engine/plugin_host/plugin_loader.h"
#include "engine/source/nexus/auth.h"
#include "engine/source/nexus/update_check.h"
#include "engine/mod/meta/mod_meta.h"
#include "engine/source/update/mod_update_db_client.h"
#include "platform/platform.h"
#include "test_platform.h"
#include "ui/controllers/downloads_controller.h"
#include "ui/controllers/mod_list_controller.h"
#include "ui/main_window/main_window.h"
#include "ui/modinfo/mod_info_data.h"
#include "ui/panels/archives_tab.h"
#include "ui/panels/conflicts_tab.h"
#include "ui/panels/data_tab.h"
#include "ui/panels/downloads_tab.h"
#include "ui/panels/plugin_view.h"
#include "ui/panels/plugins_tab.h"
#include "ui/panels/saves_tab.h"
#include "ui/settings/settings.h"
#include "ui/widgets/category_filter_panel.h"
#include "ui/widgets/console_panel.h"
#include "ui/widgets/debug_window.h"
#include "ui/widgets/exec_controls_bar.h"
#include "ui/widgets/game_path_banner.h"
#include "ui/widgets/main_toolbar.h"
#include "ui/widgets/menu_bar.h"
#include "ui/widgets/mod_filter_bar.h"
#include "ui/widgets/mod_list_model.h"
#include "ui/widgets/mod_table_view.h"
#include "ui/widgets/profile_bar.h"
#include "ui/widgets/right_filter_bar.h"
#include "ui/widgets/right_panel.h"
#include "ui/widgets/status_bar.h"
#include "ui/workers/pipeline_worker.h"

#include <QApplication>
#include <QCommandLinkButton>
#include <QContextMenuEvent>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QEvent>
#include <QHelpEvent>
#include <QItemSelectionModel>
#include <QLCDNumber>
#include <QLabel>
#include <QMenu>
#include <QModelIndex>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QToolTip>
#include <QTreeWidget>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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
class FakePlatform : public gmm_test::InertPlatform {
public:
  explicit FakePlatform(fs::path data_dir) : data_dir_(std::move(data_dir)) {}
  std::string platform_name() const override { return "fake"; }
  fs::path data_dir() const override { return data_dir_; }
  fs::path config_dir() const override { return data_dir_; }
  fs::path cache_dir() const override { return data_dir_; }
  fs::path home_dir() const override { return data_dir_; }
  fs::path temp_dir() const override { return data_dir_; }

private:
  fs::path data_dir_;
};

// The fake CLI's identifying marker: a leftover from a previous run is ours
// to replace; a foreign gmm_lootcli (a real GMM_WITH_LOOT=ON build) is not
// touched and the sort case skips instead of clobbering the artifact or
// reaching the network.
constexpr const char *kFakeLootcliMarker = "gmm_qs50 fake lootcli";

std::string fake_lootcli_script(int hold_seconds = 0) {
  // Same stdout protocol as tests/engine/loot_sorter_test.cpp: progress
  // markers, --pluginListOutputPath / --out parsing, sorted list + MO2
  // lootcli createPlugins-shape JSON report. Sorted output = every seeded
  // plugin (natives first, user band ZetaMod.esp before MyMod.esp) so
  // PluginDb::apply_load_order accepts it and the row swap is observable.
  //
  // hold_seconds sleeps before the first output. The UI-lock case needs the
  // subprocess to still be running at the instant it inspects the window, and
  // an instant stand-in would race the event loop rather than prove the lock
  // is held for the duration of the run.
  std::string s;
  s += "#!/bin/sh\n";
  s += "# gmm_qs50 fake lootcli - hermetic harness stand-in, do not run\n";
  s += std::string("echo '") + kFakeLootcliMarker + "'\n";
  if (hold_seconds > 0)
    s += "sleep " + std::to_string(hold_seconds) + "\n";
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
bool install_fake_lootcli(FakeLootcliGuard &guard, int hold_seconds = 0) {
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
  write_file(cli, fake_lootcli_script(hold_seconds));
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
  // HOME too, not just the XDG pair: QSettings resolves $HOME whenever
  // XDG_CONFIG_HOME is unset, and the freedesktop trash falls back to
  // $HOME/.local/share/Trash. Redirecting only the XDG vars leaves the real
  // user's config and trash reachable from code that misses one of them.
  qputenv("HOME", QByteArray(root.string().c_str()));
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
// lock_gui gating of the LOOT sort, driven through the real window.
//
// The Workarounds checkbox gates LockReason::LockUI (Locker::lock,
// ui_locker.cpp:20); both install sites pass OutputRequired, which overrides
// the setting, so before the LOOT sort took a gated lock the checkbox had no
// caller at all. It is the only consumer test of that gate there is, which is
// why BOTH cases live here: an on-case-only test stays green with the gate
// deleted, which is the exact defect this covers.
//
// The fake CLI holds for 2s, so the lock state read immediately after the
// click is read while gmm_lootcli is genuinely still running.
// ---------------------------------------------------------------------------
TEST_CASE("MainWindow: LOOT sort UI lock follows lock_gui", "[ui][harness]") {
  const fs::path root = make_case_root("gmm_lockgui_loot");

  const fs::path game_dir = root / "game";
  fs::create_directories(game_dir / "Data");
  write_file(game_dir / "Data" / "Skyrim.esm", "fake esm");
  write_file(game_dir / "Data" / "Update.esm", "fake esm");
  write_file(game_dir / "Data" / "MyMod.esp", "fake esp");
  write_file(game_dir / "Data" / "ZetaMod.esp", "fake esp");

  const fs::path ml_dir = root / "data" / "loot" / "testgame";
  fs::create_directories(ml_dir);
  write_file(ml_dir / "masterlist.yaml", "seq: 1\n");
  write_file(ml_dir / "prelude.yaml", "groups: []\n");

  qputenv("QT_QPA_PLATFORM", "offscreen");
  int app_argc     = 1;
  char app_argv0[] = "main_window_harness_test";
  char *app_argv[] = {app_argv0, nullptr};
  QApplication app(app_argc, app_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  FakeLootcliGuard lootcli;
  if (!install_fake_lootcli(lootcli, /*hold_seconds=*/2)) {
    WARN("a real gmm_lootcli sits next to the test binary - hermetic fake "
         "not installed; skipping the LOOT lock case");
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
  auto *rp = w.findChild<ui::RightPanel *>();
  REQUIRE(rp != nullptr);
  rp->set_capabilities(&caps);
  w.show();
  w.set_game_info("testgame", "Test Game", "Default", game_dir, inst.info().root);

  REQUIRE(pump_until([&w] {
    return !w.is_loading();
  }));

  click_tab(rp->tab_widget(), QStringLiteral("Plugins"));
  auto *ptab = rp->plugins_tab();
  REQUIRE(ptab != nullptr);
  auto *table = ptab->table();
  REQUIRE(table != nullptr);
  REQUIRE(pump_until([&] {
    return table->rowCount() == 4;
  }));

  auto *sort_btn = rp->filter_bar()->findChild<QPushButton *>();
  REQUIRE(sort_btn != nullptr);
  REQUIRE(pump_until(
      [&] {
        return sort_btn->isVisible();
      },
      5000));

  // Not SECTIONs: a Catch2 section re-runs the whole TEST_CASE body, and the
  // body owns the QApplication, which may exist only once per process. Two
  // linear blocks in one case is what puts both gate states in one run.
  //
  // run_loot_sort takes the lock synchronously on this thread before it queues
  // the worker, so the window state read on the line after the click is the
  // in-flight state, and the fake CLI's 2s hold keeps the run in flight.
  const auto sorted = [&] {
    return find_row(table, QStringLiteral("ZetaMod.esp")) == 2 &&
           find_row(table, QStringLiteral("MyMod.esp")) == 3;
  };

  // lock_gui OFF. The gate must decline, so the window stays live while
  // gmm_lootcli runs. Run first, while the plugin table is still in its
  // pre-sort order, so the row swap is an unambiguous "this run finished".
  Settings::instance().set_lock_gui(false);
  QTest::mouseClick(sort_btn, Qt::LeftButton);
  CHECK(w.centralWidget()->isEnabled());
  REQUIRE(pump_until(sorted, 30000));
  CHECK(w.centralWidget()->isEnabled());

  // lock_gui ON. Same button, same real sort, and the gate must engage. Waited
  // on the window coming back rather than on the rows: the release IS the
  // property under test, and a stranded lock would need a restart to clear.
  Settings::instance().set_lock_gui(true);
  QTest::mouseClick(sort_btn, Qt::LeftButton);
  CHECK_FALSE(w.centralWidget()->isEnabled());
  REQUIRE(pump_until(
      [&] {
        return w.centralWidget()->isEnabled();
      },
      30000));

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

// Reinstall Mod (MO2 ModListViewActions::reinstallMod): the action is on the
// real mod context menu and its missing-archive guard reports the reason
// through the shared error dialog instead of starting an install. The
// install-itself half needs a live pipeline thread, a real archive and the
// FOMOD/overwrite dialogs - covered by pipeline_worker_test and the install
// stage tests; what is proven here is that the control is reachable and that
// it refuses, loudly and without touching the mod, when there is no archive.
TEST_CASE("MainWindow: Reinstall Mod reports a missing archive", "[ui][harness]") {
  const fs::path root     = make_case_root("gmm_qs50_reinstall");
  const fs::path inst_dir = root / "instances";

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
  const auto instance      = engine::Instance::from_root(inst_root);
  const fs::path mods_dir  = instance.path_for(engine::InstanceKind::Mods);
  fs::create_directories(instance.path_for(engine::InstanceKind::Downloads));
  fs::create_directories(mods_dir / "Foo_mod");
  // Records an archive that is NOT in the downloads dir (the mod was
  // installed, then the downloads folder was cleared).
  write_file(mods_dir / "Foo_mod" / "meta.ini",
             "[General]\ninstallationFile=Foo-42.zip\n");

  engine::GameKnowledge knowledge;
  knowledge.set("testgame", "mods_subpath", "Mods");

  ui::MainWindow w;
  w.set_game_knowledge(&knowledge);
  w.show();
  w.set_game_info("testgame", "Test Game", "Default", {}, inst_root);
  REQUIRE(pump_until([&w] {
    return !w.is_loading();
  }));

  auto *view = w.mod_view();
  REQUIRE(view != nullptr);
  REQUIRE(pump_until([&] {
    return find_mod_row(view, QStringLiteral("Foo_mod")) >= 0;
  }));
  const int row = find_mod_row(view, QStringLiteral("Foo_mod"));
  view->scrollTo(view->model()->index(row, 0));
  QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  const QRect cell = view->visualRect(view->model()->index(row, 0));
  REQUIRE(cell.isValid());

  QTimer::singleShot(10000, [] {
    if (auto *popup = QApplication::activePopupWidget())
      popup->close();
    if (auto *modal = QApplication::activeModalWidget())
      modal->close();
  });

  // Armed BEFORE the menu opens: it fires inside QMenu::exec's nested loop,
  // triggers Reinstall, then reads + closes the error dialog raised by
  // ui::report_error inside the action's own modal exec loop.
  bool menu_seen = false;
  QString reported;
  QTimer::singleShot(0, [&] {
    auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
    if (!menu)
      return;
    menu_seen          = true;
    QAction *reinstall = nullptr;
    for (auto *a : menu->actions()) {
      if (a->text() == QStringLiteral("Reinstall Mod")) {
        reinstall = a;
        break;
      }
    }
    if (!reinstall) {
      menu->close();
      return;
    }
    QTimer::singleShot(0, [&] {
      auto *dlg = QApplication::activeModalWidget();
      if (!dlg)
        return;
      QStringList texts;
      for (const auto *label : dlg->findChildren<QLabel *>())
        texts << label->text();
      // TaskDialog renders .details() into a QPlainTextEdit, not a label.
      for (const auto *edit : dlg->findChildren<QPlainTextEdit *>())
        texts << edit->toPlainText();
      reported = texts.join(QLatin1Char(' '));
      dlg->close();
    });
    reinstall->trigger();
  });

  QContextMenuEvent ctx(QContextMenuEvent::Mouse, cell.center(),
                        view->viewport()->mapToGlobal(cell.center()));
  QCoreApplication::sendEvent(view->viewport(), &ctx);

  REQUIRE(menu_seen);
  // The reason reaches the user verbatim, naming the archive that is gone.
  CHECK(reported.contains(QStringLiteral("Foo-42.zip")));
  CHECK(reported.contains(QStringLiteral("downloads")));
  // Nothing was installed and nothing was removed.
  CHECK(fs::exists(mods_dir / "Foo_mod"));
  CHECK(find_mod_row(view, QStringLiteral("Foo_mod")) >= 0);

  fs::remove_all(root);
}

// ---------------------------------------------------------------------------
// Workspace-8fy: mod-list double-click - column-dependent Mod Info tab,
// separator fold toggle, anti-bounce guard.
// ---------------------------------------------------------------------------
TEST_CASE("ModList double-click column maps to Mod Info tab", "[ui][dblclick]") {
  using ui::ModInfoTabId;
  using ui::ModList;
  using ui::ModListController;
  // MO2 column -> tab mapping (GMM renames MO2's Nexus tab to Source; GMM
  // has no Notes/Game/Mod-ID columns, so those MO2 mappings have no target).
  CHECK(ModListController::mod_info_tab_for_column(ModList::Conflicts) ==
        static_cast<int>(ModInfoTabId::Conflicts));
  CHECK(ModListController::mod_info_tab_for_column(ModList::Flags) ==
        static_cast<int>(ModInfoTabId::Conflicts));
  CHECK(ModListController::mod_info_tab_for_column(ModList::Category) ==
        static_cast<int>(ModInfoTabId::Categories));
  CHECK(ModListController::mod_info_tab_for_column(ModList::Source) ==
        static_cast<int>(ModInfoTabId::Source));
  CHECK(ModListController::mod_info_tab_for_column(ModList::SourceId) ==
        static_cast<int>(ModInfoTabId::Source));
  CHECK(ModListController::mod_info_tab_for_column(ModList::Version) ==
        static_cast<int>(ModInfoTabId::Source));
  // Everything else opens the dialog on the last-used tab (-1).
  CHECK(ModListController::mod_info_tab_for_column(ModList::Fold) == -1);
  CHECK(ModListController::mod_info_tab_for_column(ModList::Name) == -1);
  CHECK(ModListController::mod_info_tab_for_column(ModList::Installation) == -1);
  CHECK(ModListController::mod_info_tab_for_column(ModList::Changed) == -1);
  CHECK(ModListController::mod_info_tab_for_column(ModList::Priority) == -1);
}

// ---------------------------------------------------------------------------
// Help on UI: the What's This text behind QWhatsThis::enterWhatsThisMode().
// Without it the mode hands the user a pointer and nothing to read, so every
// main-window surface the mode can be pointed at has to carry a string. The
// list below is the covered set, asserted by name so a surface cannot quietly
// lose its text.
// ---------------------------------------------------------------------------
TEST_CASE("MainWindow: surfaces carry What's This text for Help on UI",
          "[ui][harness][whatsthis]") {
  const fs::path root     = make_case_root("gmm_whats_this");
  const fs::path inst_dir = root / "instances";

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

  engine::GameKnowledge knowledge;
  knowledge.set("testgame", "mods_subpath", "Mods");

  ui::MainWindow w;
  w.set_game_knowledge(&knowledge);
  w.show();
  w.set_game_info("testgame", "Test Game", "Default", {}, inst.info().root);
  REQUIRE(pump_until([&w] {
    return !w.is_loading();
  }));

  auto *rp = w.findChild<ui::RightPanel *>();
  REQUIRE(rp != nullptr);
  REQUIRE(rp->data_tab() != nullptr);

  // Surface name -> widget. The containers (bars, panels, the window's own
  // chrome) carry the text themselves; Qt walks up the parent chain from
  // whatever the pointer lands on, so a container covers the controls inside
  // it, exactly as Designer-set whatsThis does in MO2's .ui files.
  const std::vector<std::pair<const char *, QWidget *>> surfaces = {{
      {"AppMenuBar", w.findChild<ui::AppMenuBar *>()},
      {"MainToolbar", w.findChild<ui::MainToolbar *>()},
      {"StatusBar", w.findChild<ui::StatusBar *>()},
      {"ConsolePanel", w.findChild<ui::ConsolePanel *>()},
      {"GamePathBanner", w.findChild<ui::GamePathBanner *>()},
      {"ProfileBar", w.findChild<ui::ProfileBar *>()},
      {"ModView", w.findChild<ui::ModView *>()},
      {"ModFilterBar", w.findChild<ui::ModFilterBar *>()},
      {"CategoryFilterPanel", w.findChild<ui::CategoryFilterPanel *>()},
      {"mod count counter", w.findChild<QLCDNumber *>()},
      {"RightPanel", rp},
      {"RightFilterBar", rp->filter_bar()},
      {"ExecControlsBar", rp->exec_controls()},
      {"DataTab tree", rp->data_tab()->tree()},
  }};

  for (const auto &[name, widget] : surfaces) {
    INFO("surface: " << name);
    REQUIRE(widget != nullptr);
    CHECK_FALSE(widget->whatsThis().trimmed().isEmpty());
  }

  // The tabs the right panel builds on first show. They carry their text in
  // their own constructors, so they are pinned here by construction rather
  // than by materializing five tabs on a wired instance - the string is the
  // thing under test, not the lazy build.
  {
    const ui::PluginsTab plugins;
    const ui::ConflictsTab conflicts;
    const ui::ArchivesTab archives;
    const ui::SavesTab saves;
    const ui::DownloadsTab downloads;
    const std::vector<std::pair<const char *, QWidget *>> tabs = {
        {"PluginsTab view", plugins.findChild<ui::PluginView *>()},
        {"ConflictsTab tree", conflicts.findChild<QTreeWidget *>()},
        {"ArchivesTab tree", archives.tree()},
        {"SavesTab table", saves.table()},
        {"DownloadsTab table", downloads.table()},
    };
    for (const auto &[name, widget] : tabs) {
      INFO("surface: " << name);
      REQUIRE(widget != nullptr);
      CHECK_FALSE(widget->whatsThis().trimmed().isEmpty());
    }
  }

  // One pinned string, so a wholesale wipe-and-refill cannot pass on a set of
  // blank-looking-but-non-empty stubs. MO2 carries the same sentence on its
  // mod list (mainwindow.ui, modList).
  auto *view = w.findChild<ui::ModView *>();
  REQUIRE(view != nullptr);
  CHECK(view->whatsThis().contains("list of installed mods"));
  CHECK(view->whatsThis().contains("activate/deactivate"));

  // The Data tab's own wording, taken from MO2's dataTree.
  CHECK(rp->data_tab()->tree()->whatsThis().contains("data directory"));

  fs::remove_all(root);
}

namespace {
// Records a modal Mod Info dialog's tab and closes it so dlg.exec()
// unwinds instead of hanging the case (the task_dialog_test pattern: the
// poll fires inside the nested exec loop).
struct ModalProbe {
  QTimer poll;
  bool dialog_seen           = false;
  int seen_tab               = -2;
  bool conflicts_tab_enabled = true;
  void reset() {
    dialog_seen           = false;
    seen_tab              = -2;
    conflicts_tab_enabled = true;
  }
  void arm() {
    poll.setInterval(25);
    QObject::connect(&poll, &QTimer::timeout, [this] {
      if (auto *modal = QApplication::activeModalWidget()) {
        dialog_seen = true;
        if (auto *tabs = modal->findChild<QTabWidget *>()) {
          seen_tab = tabs->currentIndex();
          for (int i = 0; i < tabs->count(); ++i) {
            if (tabs->tabText(i) == QStringLiteral("Conflicts"))
              conflicts_tab_enabled = tabs->isTabEnabled(i);
          }
        }
        modal->close();
      }
    });
    poll.start();
  }
  void disarm() { poll.stop(); }
};
}  // namespace

TEST_CASE("MainWindow: mod list double-click tabs, separator toggle, bounce guard",
          "[ui][harness]") {
  const fs::path root     = make_case_root("gmm_8fy_dblclick");
  const fs::path inst_dir = root / "instances";

  // QApplication must outlive everything in this case (dialogs, widgets),
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
  fs::create_directories(mods_dir / "Bar_mod");
  write_file(mods_dir / "Bar_mod" / "meta.ini", "[General]\npriority=1\n");

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
  auto *ctl = w.findChild<ui::ModListController *>();
  REQUIRE(ctl != nullptr);
  REQUIRE(pump_until([&] {
    return find_mod_row(view, QStringLiteral("Foo_mod")) >= 0;
  }));
  REQUIRE(pump_until([&] {
    return find_mod_row(view, QStringLiteral("Bar_mod")) >= 0;
  }));

  // Separator above the mods (created without prompts, then moved to row 0
  // so it has content below - the flat fold rule needs a mod underneath).
  const QString sep_id =
      ctl->create_separator_named(QStringLiteral("Group"), QString());
  REQUIRE_FALSE(sep_id.isEmpty());
  model->move_mod(sep_id, 0);
  QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

  auto find_separator_row = [&] {
    const auto &mods = model->mods();
    for (int r = 0; r < static_cast<int>(mods.size()); ++r) {
      if (mods[r].is_separator)
        return r;
    }
    return -1;
  };
  REQUIRE(find_separator_row() >= 0);
  REQUIRE(model->has_content(find_separator_row()));
  REQUIRE_FALSE(model->mods()[find_separator_row()].folded);

  // Safety watchdog: if a scripted gesture leaves a modal open, close it
  // after 10s so the case FAILs instead of hanging.
  QTimer::singleShot(10000, [] {
    if (auto *popup = QApplication::activePopupWidget())
      popup->close();
    if (auto *modal = QApplication::activeModalWidget())
      modal->close();
  });

  ModalProbe probe;
  probe.arm();

  int dblclick_count    = 0;
  int last_dblclick_row = -1;
  int last_dblclick_col = -1;
  QObject::connect(view, &QTreeView::doubleClicked, [&](const QModelIndex &idx) {
    ++dblclick_count;
    last_dblclick_row = idx.row();
    last_dblclick_col = idx.column();
  });

  auto dblclick_cell = [&](int row, int col) {
    dblclick_count = 0;
    view->scrollTo(view->model()->index(row, col));
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    const QRect cell = view->visualRect(view->model()->index(row, col));
    REQUIRE(cell.isValid());
    // Synthesized platform double-click sequence (Press, Release,
    // DblClick, Release). QTest::mouseDClick does not produce
    // doubleClicked on this view offscreen, hence the direct sequence.
    const QPoint pos    = cell.center();
    const QPoint global = view->viewport()->mapToGlobal(pos);
    QMouseEvent press(QEvent::MouseButtonPress, pos, global, Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, pos, global, Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QMouseEvent dblpress(QEvent::MouseButtonDblClick, pos, global, Qt::LeftButton,
                         Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(view->viewport(), &press);
    QCoreApplication::sendEvent(view->viewport(), &release);
    QCoreApplication::sendEvent(view->viewport(), &dblpress);
    QCoreApplication::sendEvent(view->viewport(), &release);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  };

  int foo = find_mod_row(view, QStringLiteral("Foo_mod"));
  REQUIRE(foo >= 0);

  // Column -> tab: Version opens the Source tab (GMM rename of MO2's Nexus
  // tab). Closing the dialog persists it as the last-used tab.
  probe.reset();
  dblclick_count = 0;
  dblclick_cell(foo, ui::ModList::Version);
  CHECK(dblclick_count == 1);
  CHECK(last_dblclick_row == foo);
  CHECK(last_dblclick_col == static_cast<int>(ui::ModList::Version));
  CHECK(probe.dialog_seen);
  CHECK(probe.seen_tab == static_cast<int>(ui::ModInfoTabId::Source));

  // Name opens the dialog on the last-used tab (Source, just persisted).
  foo = find_mod_row(view, QStringLiteral("Foo_mod"));
  REQUIRE(foo >= 0);
  probe.reset();
  dblclick_cell(foo, ui::ModList::Name);
  CHECK(probe.dialog_seen);
  CHECK(probe.seen_tab == static_cast<int>(ui::ModInfoTabId::Source));

  // Category -> Categories, Conflicts -> Conflicts.
  foo = find_mod_row(view, QStringLiteral("Foo_mod"));
  REQUIRE(foo >= 0);
  probe.reset();
  dblclick_cell(foo, ui::ModList::Category);
  CHECK(dblclick_count == 1);
  CHECK(last_dblclick_col == static_cast<int>(ui::ModList::Category));
  CHECK(probe.dialog_seen);
  CHECK(probe.seen_tab == static_cast<int>(ui::ModInfoTabId::Categories));

  foo = find_mod_row(view, QStringLiteral("Foo_mod"));
  REQUIRE(foo >= 0);
  probe.reset();
  dblclick_cell(foo, ui::ModList::Conflicts);
  CHECK(dblclick_count == 1);
  CHECK(last_dblclick_col == static_cast<int>(ui::ModList::Conflicts));
  CHECK(probe.dialog_seen);
  // Foo is contentless, so the dialog disables the empty Conflicts tab
  // and Qt advances to the next enabled tab (Categories). The
  // column -> Conflicts mapping itself is pinned by the unit test above;
  // here the click chain (Conflicts column -> dialog) plus the
  // disabled-for-empty Conflicts tab cover the integration.
  CHECK_FALSE(probe.conflicts_tab_enabled);

  // Separator double-click toggles the fold (MO2 parity) and never opens a
  // dialog - twice, so it folds then unfolds.
  int sep = find_separator_row();
  REQUIRE(sep >= 0);
  probe.reset();
  dblclick_cell(sep, ui::ModList::Name);
  CHECK_FALSE(probe.dialog_seen);
  CHECK(model->mods()[find_separator_row()].folded);

  sep = find_separator_row();
  REQUIRE(sep >= 0);
  probe.reset();
  dblclick_cell(sep, ui::ModList::Name);
  CHECK_FALSE(probe.dialog_seen);
  CHECK_FALSE(model->mods()[find_separator_row()].folded);

  // Anti-bounce: a real checkbox click immediately before the
  // double-click swallows it (the second half of a checkbox aim, not an
  // open request). The click targets the checkbox at the left of the Name
  // cell; the toggle is verified through the model before double-clicking.
  foo = find_mod_row(view, QStringLiteral("Foo_mod"));
  REQUIRE(foo >= 0);
  view->scrollTo(view->model()->index(foo, ui::ModList::Name));
  QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  const QRect name_cell =
      view->visualRect(view->model()->index(foo, ui::ModList::Name));
  REQUIRE(name_cell.isValid());
  QModelIndex name_idx    = view->model()->index(foo, ui::ModList::Name);
  const auto before_click = static_cast<Qt::CheckState>(
      view->model()->data(name_idx, Qt::CheckStateRole).toInt());
  QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier,
                    QPoint(name_cell.left() + 8, name_cell.center().y()));
  QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  const auto after_click = static_cast<Qt::CheckState>(
      view->model()->data(name_idx, Qt::CheckStateRole).toInt());
  REQUIRE(after_click != before_click);
  probe.reset();
  dblclick_cell(foo, ui::ModList::Version);
  CHECK_FALSE(probe.dialog_seen);

  // Control: the same cell opens the dialog once the interval has passed, so
  // the CHECK above is the guard and not a dead gesture.
  pump_ms(QApplication::doubleClickInterval() + 50);
  probe.reset();
  dblclick_cell(foo, ui::ModList::Version);
  CHECK(probe.dialog_seen);

  probe.disarm();
  fs::remove_all(root);
}

// Settings > Mod List > "Display foreign mods (DLC, Creation Club)".
//
// The setting decides whether the game-native rows a scan synthesizes for
// content the game manages itself appear in the mod list. The case drives the
// real window with a real game dir, so the row really comes from the scanner
// and really goes through the filter - a stub would prove nothing.
TEST_CASE("MainWindow: display_foreign hides the game-native rows",
          "[ui][settings][harness]") {
  const fs::path root     = make_case_root("gmm_qs50_foreign");
  const fs::path inst_dir = root / "instances";

  qputenv("QT_QPA_PLATFORM", "offscreen");
  int app_argc     = 1;
  char app_argv0[] = "main_window_harness_test";
  char *app_argv[] = {app_argv0, nullptr};
  QApplication app(app_argc, app_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  // The knowledge key the mod scanner reads to synthesize the declared
  // game-native plugin rows (engine::native_plugins_csv).
  engine::GameKnowledge knowledge;
  knowledge.set("testgame", "mods_subpath", "Mods");
  knowledge.set("testgame", "game_native_plugins", "TestGame.esm");

  auto inst           = engine::Instance::installed("TestGame", inst_dir);
  inst.info().game_id = "testgame";
  REQUIRE(inst.create_directories());
  REQUIRE(inst.write_toml());
  const fs::path inst_root = inst.info().root;
  const fs::path mods_dir =
      engine::Instance::from_root(inst_root).path_for(engine::InstanceKind::Mods);
  fs::create_directories(mods_dir / "Foo_mod");
  write_file(mods_dir / "Foo_mod" / "meta.ini", "[General]\npriority=0\n");

  // The declared native plugin, present where the scanner resolves natives
  // from for this instance: it synthesizes a game-native row for it.
  const fs::path game_dir = root / "game";
  fs::create_directories(game_dir / "Mods");
  write_file(game_dir / "Mods" / "TestGame.esm", std::string("\0\0\0\0", 4));

  ui::MainWindow w;
  w.set_game_knowledge(&knowledge);
  w.show();
  w.set_game_info("testgame", "Test Game", "Default", game_dir, inst_root);
  REQUIRE(pump_until([&w] {
    return !w.is_loading();
  }));

  auto *view = w.mod_view();
  REQUIRE(view != nullptr);
  auto *model = w.findChild<ui::ModList *>();
  REQUIRE(model != nullptr);

  // A game-native row shows as "Unmanaged: <file>", so it is located by the
  // model's id rather than by a Name-cell text match.
  const auto row_of = [&](const QString &id) {
    for (int r = 0; r < model->rowCount(); ++r)
      if (model->mods().at(r).id == id)
        return r;
    return -1;
  };
  REQUIRE(pump_until([&] {
    return row_of(QStringLiteral("Foo_mod")) >= 0;
  }));

  // Both rows are listed, and both visible, with the setting on (the stored
  // default). isRowHidden is the same call the filter makes.
  REQUIRE(Settings::instance().display_foreign());
  const int foo_on    = row_of(QStringLiteral("Foo_mod"));
  const int native_on = row_of(QStringLiteral("TestGame.esm"));
  REQUIRE(foo_on >= 0);
  REQUIRE(native_on >= 0);
  CHECK(!view->isRowHidden(foo_on, QModelIndex()));
  CHECK(!view->isRowHidden(native_on, QModelIndex()));

  // The setting off: the foreign row leaves the list, the managed mod stays.
  Settings::instance().set_display_foreign(false);
  auto *mlc = w.findChild<ui::ModListController *>();
  REQUIRE(mlc != nullptr);
  mlc->apply_mod_filter();

  const int foo_off    = row_of(QStringLiteral("Foo_mod"));
  const int native_off = row_of(QStringLiteral("TestGame.esm"));
  REQUIRE(foo_off >= 0);
  REQUIRE(native_off >= 0);
  CHECK(!view->isRowHidden(foo_off, QModelIndex()));
  CHECK(view->isRowHidden(native_off, QModelIndex()));

  // Back on: the row returns, which is the same path the setting takes when a
  // user re-checks the box.
  Settings::instance().set_display_foreign(true);
  mlc->apply_mod_filter();
  CHECK(!view->isRowHidden(row_of(QStringLiteral("TestGame.esm")), QModelIndex()));
}

// Settings > Mod List > "Check for mods for updates after install".
//
// The setting gates a MOD update poll, not an application update: MO2's
// equivalent is consumed by modInstalled, which asks the mod update database
// about the mod that was just installed. The assertion is on the consumer's
// output - whether the database was actually asked, and about which game - so
// a getter round-trip cannot satisfy it: that would pass with the poll body
// emptied.
TEST_CASE("MainWindow: check_update_after_mod_install gates the mod update poll",
          "[ui][settings][harness]") {
  const fs::path root     = make_case_root("gmm_h75m_modupdate");
  const fs::path inst_dir = root / "instances";

  qputenv("QT_QPA_PLATFORM", "offscreen");
  int app_argc     = 1;
  char app_argv0[] = "main_window_harness_test";
  char *app_argv[] = {app_argv0, nullptr};
  QApplication app(app_argc, app_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  // skyrimspecialedition so ModUpdateDbClient::db_game_for resolves it to a
  // DB tag; an unmapped game would be skipped before the fake is reached and
  // the case would pass for the wrong reason.
  auto inst           = engine::Instance::installed("TestGame", inst_dir);
  inst.info().game_id = "skyrimspecialedition";
  REQUIRE(inst.create_directories());
  REQUIRE(inst.write_toml());

  engine::GameKnowledge knowledge;
  knowledge.set("skyrimspecialedition", "mods_subpath", "Mods");
  FakePlatform platform(root / "data");

  // Records what the poll was asked for. Returns ok=false, so nothing is
  // written to the mod update disk cache and the network is never touched.
  struct PollRecorder : public engine::update::ModUpdateDbClient {
    std::atomic<int> polls{0};
    std::string last_db_game;
    engine::update::PollResult fetch_index_impl(const std::string &db_game,
                                                const std::string &) override {
      ++polls;
      last_db_game = db_game;
      engine::update::PollResult res;
      res.last_error = "harness fake";
      return res;
    }
    engine::update::ModUpdateShardRecord
    fetch_shard_record_impl(const std::string &, std::int64_t) override {
      return {};
    }
  };
  auto recorder = std::make_unique<PollRecorder>();
  auto *rec     = recorder.get();
  engine::update::ModUpdateDbClient::set_instance(std::move(recorder));
  struct RestoreClient {
    ~RestoreClient() { engine::update::ModUpdateDbClient::set_instance(nullptr); }
  } restore_client;

  ui::MainWindow w;
  w.set_game_knowledge(&knowledge);
  w.set_platform(&platform);
  w.show();
  w.set_game_info("skyrimspecialedition", "Skyrim", "Default", {}, inst.info().root);
  REQUIRE(pump_until([&w] {
    return !w.is_loading();
  }));
  REQUIRE(w.current_game_id() == "skyrimspecialedition");

  auto *ctl = w.findChild<ui::DownloadsController *>();
  REQUIRE(ctl != nullptr);
  auto &s = Settings::instance();

  // Setting OFF: the database is not asked at all.
  s.set_check_update_after_mod_install(false);
  ctl->check_mod_updates_after_install();
  CHECK(rec->polls.load() == 0);

  // Setting ON: the database IS asked, and about this game.
  s.set_check_update_after_mod_install(true);
  ctl->check_mod_updates_after_install();
  REQUIRE(pump_until(
      [&rec] {
        return rec->polls.load() > 0;
      },
      10000));
  CHECK(rec->last_db_game == "SE");

  // No game bound: nothing to ask about, even with the setting on. This is the
  // guard that stops a poll against an empty game id.
  w.set_game_info("", "No Game", "Default", {}, inst.info().root);
  const int before = rec->polls.load();
  ctl->check_mod_updates_after_install();
  pump_ms(150);
  CHECK(rec->polls.load() == before);

  fs::remove_all(root);
}

// ---------------------------------------------------------------------------
// Debug panel + instance switch.
//
// The panel caches a non-owning ProfileManager* that it reads from
// populate_info() (Info tab) and populate_memory(). Switching instance
// replaces the active ProfileManager and frees the old one. When the panel is
// not re-bound it keeps pointing at the freed object, and the next populate
// locks a destroyed std::mutex: pthread_mutex_lock fails EINVAL, and the
// resulting std::system_error aborts the app.
//
// Instance B deliberately carries NO debugging.enabled, so the switch takes
// the hide-the-panel branch rather than the re-bind branch - the branch the
// crash dump was taken on.
// ---------------------------------------------------------------------------
TEST_CASE("Debug panel: instance switch leaves the panel on the new ProfileManager",
          "[ui][harness][crash]") {
  const fs::path root = make_case_root("gmm_dbgprofile");

  const fs::path game_dir = root / "game";
  fs::create_directories(game_dir / "Data");
  write_file(game_dir / "Data" / "Skyrim.esm", "fake esm");

  // Instance A owns profile "Default"; the flag makes set_game_info create
  // and show the debug panel for it.
  auto inst_a           = engine::Instance::installed("GameA", root / "instances");
  inst_a.info().game_id = "testgame";
  REQUIRE(inst_a.create_directories());
  REQUIRE(inst_a.write_toml());
  REQUIRE(fs::create_directories(inst_a.info().root / "profiles" / "Default"));
  write_file(inst_a.info().root / "debugging.enabled", "enabled\n");

  // Instance B owns a differently named profile and no debugging.enabled.
  auto inst_b           = engine::Instance::installed("GameB", root / "instances");
  inst_b.info().game_id = "testgame";
  REQUIRE(inst_b.create_directories());
  REQUIRE(inst_b.write_toml());
  REQUIRE(fs::create_directories(inst_b.info().root / "profiles" / "Second"));

  qputenv("QT_QPA_PLATFORM", "offscreen");
  int app_argc     = 1;
  char app_argv0[] = "main_window_harness_test";
  char *app_argv[] = {app_argv0, nullptr};
  QApplication app(app_argc, app_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  // The Info tab's "Active profile" value: which ProfileManager the panel
  // currently reads. Scans every panel table so no private accessor is
  // needed.
  auto shown_profile = [](ui::DebugWindow *dw) {
    for (auto *table : dw->findChildren<QTableWidget *>()) {
      const int row = find_row(table, QStringLiteral("Active profile"));
      if (row >= 0 && table->item(row, 1))
        return table->item(row, 1)->text();
    }
    return QString();
  };

  engine::GameKnowledge knowledge;
  knowledge.set("testgame", "mods_subpath", "Mods");

  FakePlatform platform(root / "data");
  ui::MainWindow w;
  w.set_game_knowledge(&knowledge);
  w.set_platform(&platform);
  w.show();
  w.set_game_info("testgame", "Test Game", "Default", game_dir, inst_a.info().root);
  REQUIRE(pump_until([&w] {
    return !w.is_loading();
  }));

  auto *dw = w.findChild<ui::DebugWindow *>();
  REQUIRE(dw != nullptr);
  // Precondition: the panel is bound to instance A's profile, so the
  // post-switch assertion cannot pass vacuously.
  REQUIRE(shown_profile(dw) == QStringLiteral("Default"));

  // The switch: load_mods_from_game() replaces - and frees - the active
  // ProfileManager.
  w.set_game_info("testgame", "Test Game", "Second", game_dir, inst_b.info().root);
  REQUIRE(pump_until([&w] {
    return !w.is_loading();
  }));

  // The panel must already track instance B's profile. Checked before the
  // reopen because this reads a plain Qt cell - no freed memory - so the
  // failure is a plain value mismatch rather than a lock on a destroyed
  // mutex (which spins forever).
  REQUIRE(shown_profile(dw) == QStringLiteral("Second"));

  // Re-opening is what ran the freed ProfileManager: showEvent re-runs
  // populate_info(), which locks that object's mods_mutex_.
  dw->hide();
  dw->show();
  pump_ms(50);

  REQUIRE(shown_profile(dw) == QStringLiteral("Second"));

  fs::remove_all(root);
}

// MO2 ModListContextMenu nests the whole-list menu in every row menu as
// "All Mods" (modlistcontextmenu.cpp:230-234), and its Enable/Disable pair
// relabels itself and acts on the filtered set when a filter is active
// (modlistcontextmenu.cpp:73-87). Driven through the real context menu: the
// submenu has to be reachable, its wording has to follow the filter, and the
// action has to enable the FILTERED mods and no others.
TEST_CASE("MainWindow: All Mods menu enables only the filter-matching mods",
          "[ui][harness]") {
  const fs::path root     = make_case_root("gmm_qs50_allmods");
  const fs::path inst_dir = root / "instances";

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
  fs::create_directories(mods_dir / "Bar_mod");
  write_file(mods_dir / "Bar_mod" / "meta.ini", "[General]\npriority=1\n");
  fs::create_directories(mods_dir / "Baz_mod");
  write_file(mods_dir / "Baz_mod" / "meta.ini", "[General]\npriority=2\n");

  engine::GameKnowledge knowledge;
  knowledge.set("testgame", "mods_subpath", "Mods");

  ui::MainWindow w;
  w.set_game_knowledge(&knowledge);
  w.show();
  w.set_game_info("testgame", "Test Game", "Default", {}, inst_root);
  REQUIRE(pump_until([&w] {
    return !w.is_loading();
  }));

  auto *model = w.findChild<ui::ModList *>();
  REQUIRE(model != nullptr);
  auto *view = w.mod_view();
  REQUIRE(view != nullptr);
  REQUIRE(pump_until([&] {
    return find_mod_row(view, QStringLiteral("Foo_mod")) >= 0 &&
           find_mod_row(view, QStringLiteral("Bar_mod")) >= 0 &&
           find_mod_row(view, QStringLiteral("Baz_mod")) >= 0;
  }));

  // Start every mod disabled, written through the model's own CheckStateRole
  // path - the same write a checkbox click makes.
  auto set_enabled = [&](const QString &id, bool on) {
    const int row = find_mod_row(view, id);
    REQUIRE(row >= 0);
    model->setData(model->index(row, ui::ModList::Name),
                   on ? Qt::Checked : Qt::Unchecked, Qt::CheckStateRole);
  };
  auto mod_enabled = [&](const QString &id) {
    const int row = find_mod_row(view, id);
    REQUIRE(row >= 0);
    return model->mods()[static_cast<size_t>(row)].enabled;
  };
  set_enabled(QStringLiteral("Foo_mod"), false);
  set_enabled(QStringLiteral("Bar_mod"), false);
  set_enabled(QStringLiteral("Baz_mod"), false);

  auto *filter_bar = w.findChild<ui::ModFilterBar *>();
  REQUIRE(filter_bar != nullptr);

  // Opens the row context menu at `id`, hands the "All Mods" submenu's action
  // named `action` to `body`, and closes the menu. Returns the submenu so the
  // caller can assert the wording.
  auto run_all_mods_action = [&](const QString &id, std::function<void(QMenu &)> body) {
    const int row = find_mod_row(view, id);
    REQUIRE(row >= 0);
    const QRect cell = view->visualRect(view->model()->index(row, 0));
    REQUIRE(cell.isValid());

    QMenu *menu     = nullptr;
    QMenu *all_mods = nullptr;
    QTimer::singleShot(0, [&] {
      menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
      if (!menu) {
        return;
      }
      for (auto *a : menu->actions()) {
        if (a->text() == QStringLiteral("All Mods") && a->menu()) {
          all_mods = a->menu();
          break;
        }
      }
      if (all_mods)
        body(*all_mods);
      menu->close();
    });
    QContextMenuEvent ctx(QContextMenuEvent::Mouse, cell.center(),
                          view->viewport()->mapToGlobal(cell.center()));
    QCoreApplication::sendEvent(view->viewport(), &ctx);
    REQUIRE(menu != nullptr);
    REQUIRE(all_mods != nullptr);
  };

  auto find_action = [](QMenu &menu, const char *text) {
    for (auto *a : menu.actions()) {
      if (a->text() == QLatin1String(text))
        return a;
    }
    return static_cast<QAction *>(nullptr);
  };

  // --- Filtered half: only Foo_mod matches "Foo".
  // Filter to "Foo" through the same path the user's keystrokes take.
  {
    auto *edit = filter_bar->findChild<QLineEdit *>();
    REQUIRE(edit != nullptr);
    edit->setText(QStringLiteral("Foo"));
  }
  REQUIRE(pump_until([&] {
    return view->isRowHidden(find_mod_row(view, QStringLiteral("Bar_mod")),
                             QModelIndex()) &&
           view->isRowHidden(find_mod_row(view, QStringLiteral("Baz_mod")),
                             QModelIndex()) &&
           !view->isRowHidden(find_mod_row(view, QStringLiteral("Foo_mod")),
                              QModelIndex());
  }));

  run_all_mods_action(QStringLiteral("Foo_mod"), [&](QMenu &all_mods) {
    // Wording follows the filter (MO2 modlistcontextmenu.cpp:74-78).
    REQUIRE(find_action(all_mods, "Enable All Matching Mods") != nullptr);
    REQUIRE(find_action(all_mods, "Disable All Matching Mods") != nullptr);
    REQUIRE(find_action(all_mods, "Enable All") == nullptr);
    find_action(all_mods, "Enable All Matching Mods")->trigger();
  });

  // Consumer check: the filtered mod flipped, the two filtered-out mods did
  // not. If the action ignored the filter every mod would be enabled and the
  // two CHECK_FALSE lines are what would fail.
  CHECK(mod_enabled(QStringLiteral("Foo_mod")));
  CHECK_FALSE(mod_enabled(QStringLiteral("Bar_mod")));
  CHECK_FALSE(mod_enabled(QStringLiteral("Baz_mod")));

  // --- Unfiltered half: with no filter the wording drops back and the same
  // action covers every mod. Unrelated churn stays green.
  {
    auto *edit = filter_bar->findChild<QLineEdit *>();
    REQUIRE(edit != nullptr);
    edit->clear();
  }
  REQUIRE(pump_until([&] {
    return !view->isRowHidden(find_mod_row(view, QStringLiteral("Bar_mod")),
                              QModelIndex()) &&
           !view->isRowHidden(find_mod_row(view, QStringLiteral("Baz_mod")),
                              QModelIndex());
  }));

  run_all_mods_action(QStringLiteral("Foo_mod"), [&](QMenu &all_mods) {
    REQUIRE(find_action(all_mods, "Enable All") != nullptr);
    REQUIRE(find_action(all_mods, "Enable All Matching Mods") == nullptr);
    find_action(all_mods, "Enable All")->trigger();
  });

  CHECK(mod_enabled(QStringLiteral("Foo_mod")));
  CHECK(mod_enabled(QStringLiteral("Bar_mod")));
  CHECK(mod_enabled(QStringLiteral("Baz_mod")));

  fs::remove_all(root);
}

// ---------------------------------------------------------------------------
// The Nexus update sweep DRIVER (Workspace-sc4z).
//
// The feature shipped inert once already: the engine's UpdateCheck::run() and
// the status bar's set_update_notice() both existed with no caller, so no
// verdict was ever produced and no notice ever appeared. This case is the
// regression guard for exactly that shape - it triggers the REAL menu action
// and requires a notice that only a real Report off the worker thread can
// produce.
//
// The notice text asserted here is reached only through
// on_source_update_finished()'s "ok && message empty && total == 0" branch, and
// this instance deliberately holds NO mod from a checkable source - which is
// also what keeps the sweep at zero requests. Nothing in this case reaches
// api.nexusmods.com.
// ---------------------------------------------------------------------------
TEST_CASE("MainWindow: Check for Mod Updates runs the sweep and reports",
          "[ui][harness]") {
  const fs::path root     = make_case_root("gmm_qs50_nexusupd");
  const fs::path inst_dir = root / "instances";

  qputenv("QT_QPA_PLATFORM", "offscreen");
  int app_argc     = 1;
  char app_argv0[] = "main_window_harness_test";
  char *app_argv[] = {app_argv0, nullptr};
  QApplication app(app_argc, app_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  // A test key, written through the production keyring path. Auth resolves its
  // fallback store from XDG_CONFIG_HOME, which make_case_root just pointed into
  // this case's throwaway root, so the blob lands there and never in the real
  // ~/.config. It is not a valid key - nothing can authenticate with it - and
  // the sweep below never sends it anywhere, because the instance has no Nexus
  // mods and run() returns before the first request.
  engine::Source::Nexus::Auth::instance().set_api_key("harness-not-a-real-key");
  REQUIRE(engine::Source::Nexus::Auth::instance().has_api_key());

  auto inst           = engine::Instance::installed("TestGame", inst_dir);
  inst.info().game_id = "testgame";
  REQUIRE(inst.create_directories());
  REQUIRE(inst.write_toml());
  const fs::path inst_root = inst.info().root;
  const fs::path mods_dir =
      engine::Instance::from_root(inst_root).path_for(engine::InstanceKind::Mods);
  fs::create_directories(mods_dir / "PlainMod");
  write_file(mods_dir / "PlainMod" / "meta.ini", "[General]\npriority=0\n");

  engine::GameKnowledge knowledge;
  knowledge.set("testgame", "mods_subpath", "Mods");

  // The game must declare a Nexus domain: current_nexus_domain() reads it off
  // the registered plugin identity, and without one the Nexus part of the
  // sweep is skipped with a note rather than run.
  engine::PluginLoader loader;
  engine::PluginInfo info;
  info.game_id      = "testgame";
  info.game_support = true;
  info.nexus_domain = "skyrimspecialedition";
  loader.plugins_mutable().push_back(info);

  ui::MainWindow w;
  w.set_game_knowledge(&knowledge);
  w.set_plugin_loader(&loader);
  w.show();
  w.set_game_info("testgame", "Test Game", "Default", {}, inst_root);
  REQUIRE(pump_until([&w] {
    return !w.is_loading();
  }));

  auto *view = w.mod_view();
  REQUIRE(view != nullptr);
  REQUIRE(pump_until([&] {
    return find_mod_row(view, QStringLiteral("PlainMod")) >= 0;
  }));

  auto *bar = w.findChild<ui::StatusBar *>();
  REQUIRE(bar != nullptr);
  // Nothing has asked Nexus anything yet: the notice starts empty.
  CHECK(bar->update_notice().isEmpty());

  // The real gesture: the row context menu's "All Mods" submenu.
  const int row = find_mod_row(view, QStringLiteral("PlainMod"));
  REQUIRE(row >= 0);
  const QRect cell = view->visualRect(view->model()->index(row, 0));
  REQUIRE(cell.isValid());

  QMenu *menu     = nullptr;
  QMenu *all_mods = nullptr;
  QTimer::singleShot(0, [&] {
    menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
    if (!menu)
      return;
    for (auto *a : menu->actions())
      if (a->text() == QStringLiteral("All Mods") && a->menu()) {
        all_mods = a->menu();
        break;
      }
    if (all_mods) {
      for (auto *a : all_mods->actions())
        if (a->text() == QStringLiteral("Check for Mod Updates")) {
          a->trigger();
          break;
        }
    }
    menu->close();
  });
  QContextMenuEvent ctx(QContextMenuEvent::Mouse, cell.center(),
                        view->viewport()->mapToGlobal(cell.center()));
  QCoreApplication::sendEvent(view->viewport(), &ctx);
  REQUIRE(menu != nullptr);
  REQUIRE(all_mods != nullptr);

  // The sweep runs on the worker thread and its Report comes back through a
  // queued signal, so pump the loop rather than assuming it landed.
  REQUIRE(pump_until([&] {
    return !bar->update_notice().isEmpty();
  }));

  // A sweep that completed and had nothing of ours at any source: an answer,
  // not a warning. The wording matters - if the driver had reported a failure
  // here the user would see a warning over an instance with no checkable mods
  // in it.
  CHECK_FALSE(bar->update_notice_is_warning());
  CHECK(bar->update_notice().contains(QStringLiteral("No mods from a source")));

  fs::remove_all(root);
}

// ---------------------------------------------------------------------------
// The TOOLBAR button for the same sweep.
//
// The sweep shipped reachable only from the mod list context menu, so a user
// who never right-clicks the mod list never learns the feature exists. This
// asserts the button is WIRED to the proven path and not merely present: it
// clicks the real toolbar button and requires the PERSISTENT update notice.
//
// The button is the FORCED entry point (the context menu entry keeps the cheap
// cached sweep). Forcing only changes how many mods the sweep asks the sources
// about, and this instance has no mod from a checkable source at all, so both
// entries reach the same proven path here - which is what this test is about.
//
// update_notice() is the assertion that carries the weight. It is set only by
// set_update_notice(), which on this path is called from
// ModListController::on_source_update_finished() with a result that came back
// off the SourceUpdateThread. The transient StatusBar::set_status() text a
// guard writes cannot produce it, so every early return in check_mod_updates()
// - no instance, already running - leaves the notice empty and fails here. A
// decorative button, or one whose connect was dropped, fails the same way.
// ---------------------------------------------------------------------------
TEST_CASE("MainWindow: the toolbar update button runs the same proven sweep",
          "[ui][harness]") {
  const fs::path root     = make_case_root("gmm_qs50_nexusupdbtn");
  const fs::path inst_dir = root / "instances";

  qputenv("QT_QPA_PLATFORM", "offscreen");
  int app_argc     = 1;
  char app_argv0[] = "main_window_harness_test";
  char *app_argv[] = {app_argv0, nullptr};
  QApplication app(app_argc, app_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  // Same reasoning as the menu case above: a key that is not a real key, in a
  // throwaway XDG_CONFIG_HOME, and an instance with no mod from a checkable
  // source, so the sweep answers from its own logic and never reaches
  // api.nexusmods.com.
  engine::Source::Nexus::Auth::instance().set_api_key("harness-not-a-real-key");
  REQUIRE(engine::Source::Nexus::Auth::instance().has_api_key());

  auto inst           = engine::Instance::installed("TestGame", inst_dir);
  inst.info().game_id = "testgame";
  REQUIRE(inst.create_directories());
  REQUIRE(inst.write_toml());
  const fs::path inst_root = inst.info().root;
  const fs::path mods_dir =
      engine::Instance::from_root(inst_root).path_for(engine::InstanceKind::Mods);
  fs::create_directories(mods_dir / "PlainMod");
  write_file(mods_dir / "PlainMod" / "meta.ini", "[General]\npriority=0\n");

  engine::GameKnowledge knowledge;
  knowledge.set("testgame", "mods_subpath", "Mods");

  engine::PluginLoader loader;
  engine::PluginInfo info;
  info.game_id      = "testgame";
  info.game_support = true;
  info.nexus_domain = "skyrimspecialedition";
  loader.plugins_mutable().push_back(info);

  ui::MainWindow w;
  w.set_game_knowledge(&knowledge);
  w.set_plugin_loader(&loader);
  w.show();
  w.set_game_info("testgame", "Test Game", "Default", {}, inst_root);
  REQUIRE(pump_until([&w] {
    return !w.is_loading();
  }));

  auto *bar = w.findChild<ui::StatusBar *>();
  REQUIRE(bar != nullptr);
  auto *toolbar = w.findChild<ui::MainToolbar *>();
  REQUIRE(toolbar != nullptr);
  CHECK(bar->update_notice().isEmpty());

  // The real button, found the way a user finds it: by what the toolbar built.
  // The tooltip is the lookup key AND the only label the button has (it is
  // ToolButtonIconOnly), so it states what the click actually does: this is
  // the FORCED sweep, which ignores the per-mod lastnexusquery cache.
  QToolButton *check_updates_btn = nullptr;
  for (auto *b : toolbar->findChildren<QToolButton *>())
    if (b->toolTip() == QStringLiteral("Check every mod for updates")) {
      check_updates_btn = b;
      break;
    }
  REQUIRE(check_updates_btn != nullptr);
  REQUIRE(check_updates_btn->isEnabled());

  check_updates_btn->click();

  REQUIRE(pump_until([&] {
    return !bar->update_notice().isEmpty();
  }));

  // Same verdict the menu entry produces, from the same Report: an answer, not
  // a warning.
  CHECK_FALSE(bar->update_notice_is_warning());
  CHECK(bar->update_notice().contains(QStringLiteral("No mods from a source")));

  fs::remove_all(root);
}

// ---------------------------------------------------------------------------
// The IGNORE control, and the consumer it has to reach.
//
// Landing the sweep without this would be the same defect one layer up: the
// engine wrote ignoredVersion, load_meta_for_mods() read it and the model
// honoured it, but nothing could ever SET it - a read half wired to a write
// half nobody calls. So this drives the REAL row-menu action and asserts the
// badge it is supposed to clear actually cleared.
//
// The meta.ini is seeded with exactly what a finished sweep writes, so the row
// starts badged and the sweep never runs: no request, no network, no key.
// ---------------------------------------------------------------------------
TEST_CASE("MainWindow: Ignore This Update clears the Version badge", "[ui][harness]") {
  const fs::path root     = make_case_root("gmm_nexus_ignore");
  const fs::path inst_dir = root / "instances";

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

  // What UpdateCheck::run() writes into a mod's own meta.ini when Nexus offers
  // a newer file: the engine verdict, the newest version string, and no
  // ignore yet. The source marker is [GameModManager] source_type, which is
  // the key ModMeta::source_type() reads and the only one the sweep itself
  // filters on - [General] sourceType is not it.
  fs::create_directories(mods_dir / "NexusMod");
  write_file(mods_dir / "NexusMod" / "meta.ini", "[General]\n"
                                                 "version=1.0\n"
                                                 "newestversion=2.0\n"
                                                 "nexus_update=true\n"
                                                 "\n[Nexusmods]\n"
                                                 "modid=123\n"
                                                 "fileid=456\n"
                                                 "\n[GameModManager]\n"
                                                 "source_type=nexus\n"
                                                 "source_id=123\n");

  engine::GameKnowledge knowledge;
  knowledge.set("testgame", "mods_subpath", "Mods");

  engine::PluginLoader loader;
  engine::PluginInfo info;
  info.game_id      = "testgame";
  info.game_support = true;
  info.nexus_domain = "skyrimspecialedition";
  loader.plugins_mutable().push_back(info);

  ui::MainWindow w;
  w.set_game_knowledge(&knowledge);
  w.set_plugin_loader(&loader);
  w.show();
  w.set_game_info("testgame", "Test Game", "Default", {}, inst_root);
  REQUIRE(pump_until([&w] {
    return !w.is_loading();
  }));

  auto *view = w.mod_view();
  REQUIRE(view != nullptr);
  REQUIRE(pump_until([&] {
    return find_mod_row(view, QStringLiteral("NexusMod")) >= 0;
  }));

  const int row = find_mod_row(view, QStringLiteral("NexusMod"));
  REQUIRE(row >= 0);
  auto *model     = view->model();
  const auto cell = model->index(row, ui::ModList::Version);

  // Before: the model has taken the verdict on. The TOOLTIP is the decisive
  // signal - it is text the model builds out of the verdict, with no icon asset
  // involved - and the decoration's presence says the badge cell is live.
  //
  // Deliberately NOT asserting the icon is non-null: icon packs are staged
  // beside the binary, and under ctest resources/ is not populated, so the
  // pixmap decodes to nothing even when the badge is correctly switched on.
  // Whether the artwork actually paints is the user's GUI check.
  REQUIRE(cell.data(Qt::ToolTipRole).toString() ==
          QStringLiteral("Newest version: 2.0"));
  REQUIRE(cell.data(Qt::DecorationRole).isValid());

  // The real gesture: the row context menu.
  const QRect geom = view->visualRect(model->index(row, 0));
  REQUIRE(geom.isValid());

  QMenu *menu = nullptr;
  QTimer::singleShot(0, [&] {
    menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
    if (!menu)
      return;
    for (auto *a : menu->actions())
      if (a->text() == QStringLiteral("Ignore This Update")) {
        a->trigger();
        break;
      }
    menu->close();
  });
  // Safety watchdog so a failed menu dance FAILS instead of hanging.
  QTimer::singleShot(10000, [] {
    if (auto *popup = QApplication::activePopupWidget())
      popup->close();
  });
  QContextMenuEvent ctx(QContextMenuEvent::Mouse, geom.center(),
                        view->viewport()->mapToGlobal(geom.center()));
  QCoreApplication::sendEvent(view->viewport(), &ctx);
  REQUIRE(menu != nullptr);

  // The badge clears synchronously (the handler re-reads the metas inline), so
  // pump just enough for the event that triggers it to have been delivered.
  REQUIRE(pump_until([&] {
    return !cell.data(Qt::ToolTipRole)
                .toString()
                .contains(QStringLiteral("Newest version"));
  }));
  CHECK_FALSE(cell.data(Qt::DecorationRole).isValid());

  // The persisted half: the version the user turned away from is recorded in
  // the MOD's own meta.ini, so the ignore survives a restart.
  const engine::ModMeta after = engine::ModMeta::load(mods_dir, "NexusMod");
  CHECK(after.get("General", "ignoredVersion") == "2.0");
  CHECK(after.get("General", "nexus_update") == "true");  // the verdict is kept

  // And the menu offers the way back, now that something is being ignored.
  auto *bar = w.findChild<ui::StatusBar *>();
  REQUIRE(bar != nullptr);
  CHECK(bar->update_notice().isEmpty());  // ignoring one mod says nothing global

  fs::remove_all(root);
}

// ---------------------------------------------------------------------------
// The branch ORDER in on_source_update_finished().
//
// Three branches, and the order between them IS the feature:
//
//   any outcome !ok    a source refused or its answer was unreadable
//   skip notes         sources that could not be asked at all - a partial
//                      answer, and the checked sources' counts ride along
//   updates > 0        a count, which is the only non-warning we may print
//   otherwise         "all up to date"
//
// A sweep that could not answer must never be rendered in the shape of a sweep
// that found nothing, and a sweep that found nothing must never borrow the
// count's voice. Hoisting `updates > 0` above the two guards is the mutation
// that breaks exactly that: a refusal that also counted 3 updates prints "3
// mod(s) have an update available" as a quiet, non-warning line.
//
// It had no test, and hoisting it left every case in this file green - the
// cases that existed only ever produced one branch at a time. The two results
// below are the ones the sweeps cannot currently emit (a refusal cannot arrive
// with updates>0, because the early returns happen before the counting loop),
// so they are handed to the slot directly with the generation it expects.
//
// Each case is asserted as a WARNING whose text is neither the count form nor
// the up-to-date form, because "is it a warning" alone does not catch a
// reordering that still reached a warning by a different route.
// ---------------------------------------------------------------------------
TEST_CASE(
    "MainWindow: a refusing or partial source sweep never wears the count's voice",
    "[ui][harness]") {
  using engine::Source::SourceUpdateReport;
  using ui::SourceUpdateOutcome;
  using ui::SourceUpdateResult;

  const fs::path root     = make_case_root("gmm_nexus_order");
  const fs::path inst_dir = root / "instances";

  qputenv("QT_QPA_PLATFORM", "offscreen");
  int app_argc     = 1;
  char app_argv0[] = "main_window_harness_test";
  char *app_argv[] = {app_argv0, nullptr};
  QApplication app(app_argc, app_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  engine::Source::Nexus::Auth::instance().set_api_key("harness-not-a-real-key");

  auto inst           = engine::Instance::installed("TestGame", inst_dir);
  inst.info().game_id = "testgame";
  REQUIRE(inst.create_directories());
  REQUIRE(inst.write_toml());
  const fs::path inst_root = inst.info().root;
  const fs::path mods_dir =
      engine::Instance::from_root(inst_root).path_for(engine::InstanceKind::Mods);
  fs::create_directories(mods_dir / "PlainMod");
  write_file(mods_dir / "PlainMod" / "meta.ini", "[General]\npriority=0\n");

  engine::GameKnowledge knowledge;
  knowledge.set("testgame", "mods_subpath", "Mods");

  engine::PluginLoader loader;
  engine::PluginInfo info;
  info.game_id      = "testgame";
  info.game_support = true;
  info.nexus_domain = "skyrimspecialedition";
  loader.plugins_mutable().push_back(info);

  ui::MainWindow w;
  w.set_game_knowledge(&knowledge);
  w.set_plugin_loader(&loader);
  w.show();
  w.set_game_info("testgame", "Test Game", "Default", {}, inst_root);
  REQUIRE(pump_until([&w] {
    return !w.is_loading();
  }));

  auto *bar = w.findChild<ui::StatusBar *>();
  REQUIRE(bar != nullptr);
  auto *controller = w.findChild<ui::ModListController *>();
  REQUIRE(controller != nullptr);

  const auto finish = [&w, controller](SourceUpdateResult result) {
    controller->on_source_update_finished(std::move(result),
                                          w.source_update_generation());
  };

  // One source's outcome, in the shape the worker emits.
  const auto one_outcome = [](const char *source, SourceUpdateReport report) {
    SourceUpdateResult result;
    SourceUpdateOutcome outcome;
    outcome.source_type = source;
    outcome.report      = std::move(report);
    result.outcomes.push_back(std::move(outcome));
    return result;
  };

  // Neither of these two may read as a count or as "up to date".
  const auto refuse_but_counted = [&] {
    SourceUpdateReport r;
    r.ok      = false;
    r.updates = 3;
    r.total   = 64;
    r.message = "Nexus refused the update check: API request limit reached. Whether "
                "updates exist is unknown.";
    return one_outcome("nexus", std::move(r));
  };
  const auto partial_but_counted = [&] {
    SourceUpdateReport r;
    r.ok      = true;
    r.updates = 2;
    r.total   = 64;
    r.message = "1 mod could not be reached on Nexus and will be retried: "
                "BodySlide (the request never reached Nexus)";
    return one_outcome("nexus", std::move(r));
  };

  // The refusal first: ok=false, a message, AND three updates counted. Hoisting
  // the count branch above the guards turns this into a quiet "3 mod(s) have an
  // update available" - asserting the text is the message is what fails then.
  finish(refuse_but_counted());
  CHECK(bar->update_notice_is_warning());
  CHECK(bar->update_notice() ==
        QStringLiteral("Nexus refused the update check: API request limit "
                       "reached. Whether updates exist is unknown."));
  CHECK_FALSE(bar->update_notice().contains(QStringLiteral("3 mod")));
  CHECK_FALSE(bar->update_notice().contains(QStringLiteral("up to date")));

  // Then the partial answer: ok, a message, and two real updates. The mods that
  // could not be reached are named - a count would tell the user the sweep was
  // complete, which is the one thing it was not.
  finish(partial_but_counted());
  CHECK(bar->update_notice_is_warning());
  CHECK(bar->update_notice() ==
        QStringLiteral("1 mod could not be reached on Nexus and will be retried: "
                       "BodySlide (the request never reached Nexus)"));
  CHECK_FALSE(bar->update_notice().contains(QStringLiteral("2 mod")));
  CHECK_FALSE(bar->update_notice().contains(QStringLiteral("up to date")));

  // And the branches that may speak: with no message and updates counted, the
  // count is the only correct notice, so the guards must not be swallowing the
  // happy path either.
  {
    SourceUpdateReport r;
    r.ok      = true;
    r.updates = 2;
    r.total   = 64;
    finish(one_outcome("nexus", std::move(r)));
  }
  CHECK_FALSE(bar->update_notice_is_warning());
  CHECK(bar->update_notice().contains(QStringLiteral("2 mod")));
  CHECK_FALSE(bar->update_notice().contains(QStringLiteral("up to date")));

  // And a complete answer with nothing found, which is the only thing allowed
  // to say "up to date".
  {
    SourceUpdateReport r;
    r.ok    = true;
    r.total = 64;
    finish(one_outcome("nexus", std::move(r)));
  }
  CHECK_FALSE(bar->update_notice_is_warning());
  CHECK(bar->update_notice().contains(QStringLiteral("up to date")));

  fs::remove_all(root);
}

// ---------------------------------------------------------------------------
// The install completed and the Downloads panel has to show it.
//
// An install started from the MOD LIST (Reinstall, ModActions::reinstall_mod)
// arrives at install_complete carrying the mod FOLDER as its id, not the
// download row's key, so a mark_installed keyed on that id silently matched
// nothing: the install wrote the mod to disk and the row the archive belongs
// to went on reading "Install" for the rest of the session. MO2 resolves the
// row from the archive it installed (organizercore.cpp:945 ->
// DownloadManager::markInstalled(QString), downloadmanager.cpp:1556-1560,
// indexByName) and never from an install id at all.
//
// The assertion is the PANEL's rendered Status cell, not the persisted meta:
// the meta was already correct while the user watched a stale row.
// ---------------------------------------------------------------------------
TEST_CASE("MainWindow: an install from the mod list reaches the Downloads panel row",
          "[ui][harness][downloads]") {
  const fs::path root     = make_case_root("gmm_dl_install_row");
  const fs::path inst_dir = root / "instances";

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
  const auto instance      = engine::Instance::from_root(inst_root);
  const fs::path mods_dir  = instance.path_for(engine::InstanceKind::Mods);
  const fs::path dl_dir    = instance.path_for(engine::InstanceKind::Downloads);
  fs::create_directories(dl_dir);
  fs::create_directories(mods_dir / "Foo_mod");

  // The archive the mod was installed from, with its download meta beside it -
  // the state a finished, not-yet-installed download restores as.
  const fs::path archive = dl_dir / "Foo-42.zip";
  write_file(archive, "a real archive would be here");
  ui::DownloadMeta meta;
  meta.archive       = archive;
  meta.id            = "42-9";
  meta.name          = "Foo";
  meta.source        = "Nexus Mods";
  meta.parent_mod_id = "42";
  meta.file_id       = 9;
  meta.nexus_domain  = "testgame";
  meta.total_size    = static_cast<int64_t>(std::filesystem::file_size(archive));
  meta.state         = ui::DownloadState::Complete;
  REQUIRE(ui::write_download_meta(meta, ui::download_meta_path(meta, dl_dir)));

  engine::GameKnowledge knowledge;
  knowledge.set("testgame", "mods_subpath", "Mods");

  ui::MainWindow w;
  w.set_game_knowledge(&knowledge);
  w.show();
  w.set_game_info("testgame", "Test Game", "Default", {}, inst_root);
  REQUIRE(pump_until([&w] {
    return !w.is_loading();
  }));

  auto *panel = w.findChild<ui::RightPanel *>();
  REQUIRE(panel != nullptr);
  auto *tab = panel->ensure_downloads_tab();
  REQUIRE(tab != nullptr);
  auto *table = tab->table();
  REQUIRE(table != nullptr);

  // The row restored from its own meta, and it reads the action still
  // available - the pre-fix state the user reported.
  int row = -1;
  for (int r = 0; r < table->rowCount(); ++r) {
    auto *name = table->item(r, ui::DownloadsTab::Name);
    if (name && name->text() == QLatin1String("Foo")) {
      row = r;
      break;
    }
  }
  REQUIRE(row >= 0);
  auto *status = table->item(row, ui::DownloadsTab::Status);
  REQUIRE(status != nullptr);
  REQUIRE(status->text() == QLatin1String("Install"));

  // Exactly what the reinstall path delivers: the mod folder as the id, the
  // archive it installed as the lookup. "Hide installed" is forced OFF so the
  // assertion is about the cell's text and not about the row being hidden.
  Settings::instance().set_hide_installed_downloads(false);
  tab->mark_installed(QStringLiteral("Foo_mod").toStdString(),
                      archive.filename().string());
  QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

  // THE assertion: the panel reads Installed. Compared as std::string so a
  // failure prints the label instead of QString's internal encoding.
  CHECK(table->item(row, ui::DownloadsTab::Status)->text().toStdString() ==
        std::string("Installed"));

  fs::remove_all(root);
}

// A mod installed over an existing folder keeps its row but the folder's
// meta.ini was just rewritten - the row has to follow, or it keeps showing
// the version and badges of the install it replaced. Only the free part of
// the rescan: what ModScanner already read off the folder.
TEST_CASE("MainWindow: re-installing over an existing row refreshes that row's data",
          "[ui][harness]") {
  const fs::path root     = make_case_root("gmm_overwrite_row_refresh");
  const fs::path inst_dir = root / "instances";

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
  write_file(mods_dir / "Foo_mod" / "Foo.esp", "plugin");
  write_file(mods_dir / "Foo_mod" / "meta.ini",
             "[General]\nversion=1.24.0\ninstalled=2026-01-01T00:00:00\n"
             "[GameModManager]\nfolder=Foo_mod\n");

  engine::GameKnowledge knowledge;
  knowledge.set("testgame", "mods_subpath", "Mods");

  ui::MainWindow w;
  w.set_game_knowledge(&knowledge);
  w.show();
  w.set_game_info("testgame", "Test Game", "Default", {}, inst_root);
  REQUIRE(pump_until([&w] {
    return !w.is_loading();
  }));

  auto *model = w.findChild<ui::ModList *>();
  auto *ctl   = w.findChild<ui::ModListController *>();
  REQUIRE(model != nullptr);
  REQUIRE(ctl != nullptr);
  auto *view = w.mod_view();
  REQUIRE(view != nullptr);
  REQUIRE(pump_until([&] {
    return find_mod_row(view, QStringLiteral("Foo_mod")) >= 0;
  }));

  const int row = model->priority_of(QStringLiteral("Foo_mod"));
  REQUIRE(row >= 0);
  REQUIRE(model->index(row, ui::ModList::Version).data().toString().toStdString() ==
          std::string("1.24.0"));

  // What the overwrite wrote: the same folder, a newer version.
  write_file(mods_dir / "Foo_mod" / "meta.ini",
             "[General]\nversion=1.25.0\ninstalled=2026-10-14T07:56:30\n"
             "[GameModManager]\nfolder=Foo_mod\n");

  ctl->add_installed_mod("Foo_mod");
  QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

  // One row, not two - the folder already had one.
  CHECK(find_mod_row(view, QStringLiteral("Foo_mod")) >= 0);
  // ...and it describes the install that is on disk now. Before the fix only
  // the timestamps moved, so this cell still read the replaced version.
  CHECK(model->index(row, ui::ModList::Version).data().toString().toStdString() ==
        std::string("1.25.0"));

  fs::remove_all(root);
}

// MO2 ModListView::onModInstalled (modlistview.cpp:465-478): after an install
// the mod is scrolled to and selected, opening the separator band it sits in
// first. Navigation only - the mod does not move.
TEST_CASE("MainWindow: the installed mod is selected with its separator opened",
          "[ui][harness]") {
  const fs::path root     = make_case_root("gmm_install_focus");
  const fs::path inst_dir = root / "instances";

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
  write_file(mods_dir / "Foo_mod" / "Foo.esp", "plugin");
  write_file(mods_dir / "Foo_mod" / "meta.ini",
             "[General]\ninstalled=2026-10-14T07:56:30\n"
             "[GameModManager]\nfolder=Foo_mod\npriority=5\n");

  engine::GameKnowledge knowledge;
  knowledge.set("testgame", "mods_subpath", "Mods");

  ui::MainWindow w;
  w.set_game_knowledge(&knowledge);
  w.show();
  w.set_game_info("testgame", "Test Game", "Default", {}, inst_root);
  REQUIRE(pump_until([&w] {
    return !w.is_loading();
  }));

  auto *model = w.findChild<ui::ModList *>();
  auto *ctl   = w.findChild<ui::ModListController *>();
  REQUIRE(model != nullptr);
  REQUIRE(ctl != nullptr);
  auto *view = w.mod_view();
  REQUIRE(view != nullptr);
  REQUIRE(pump_until([&] {
    return find_mod_row(view, QStringLiteral("Foo_mod")) >= 0;
  }));

  // A separator above the mod, then folded: the mod is inside a band that is
  // shut, which is the state that made the scroll-to a no-op.
  const QString sep_id =
      ctl->create_separator_named(QStringLiteral("Group"), QString());
  REQUIRE_FALSE(sep_id.isEmpty());
  model->move_mod(sep_id, 0);
  QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  const int sep_row = model->priority_of(sep_id);
  REQUIRE(sep_row >= 0);
  REQUIRE(model->mods()[sep_row].is_separator);
  REQUIRE(model->has_content(sep_row));
  const int mod_row = model->priority_of(QStringLiteral("Foo_mod"));
  REQUIRE(mod_row > sep_row);

  model->set_folded(sep_row, true);
  model->apply_fold_state();
  QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  REQUIRE(model->is_row_fold_hidden(mod_row));

  ctl->focus_installed_mod("Foo_mod");
  QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

  // The separator band is open again, so the row the user has to see is
  // actually on screen.
  CHECK_FALSE(model->mods()[sep_row].folded);
  CHECK_FALSE(model->is_row_fold_hidden(mod_row));
  CHECK_FALSE(view->isRowHidden(mod_row, QModelIndex()));

  // The row is selected - scrollTo alone leaves the selection where it was,
  // which is what made this look like "nothing happened".
  CHECK(view->currentIndex().isValid());
  CHECK(view->currentIndex().row() == mod_row);
  CHECK(view->selectionModel()->isSelected(model->index(mod_row, 0)));

  // The 50 ms deferred focus MO2 uses (modlistview.cpp:432-434). Bounded, so
  // a callback that never fires FAILs the case instead of hanging it.
  QElapsedTimer focus_timer;
  focus_timer.start();
  bool focused = false;
  while (focus_timer.elapsed() < 2000 && !focused) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    focused = view->hasFocus();
  }
  CHECK(focused);

  // Navigation only: the mod is still exactly where it was.
  CHECK(model->priority_of(QStringLiteral("Foo_mod")) == mod_row);

  fs::remove_all(root);
}

// The Downloads row that was clicked, located by the archive its tooltip
// carries. Two downloads of the SAME mod share a display name, so the name
// cannot pick the row - which is precisely how the user ended up looking at
// the wrong one.
int find_download_row_for_archive(QTableWidget *table, const QString &archive) {
  for (int r = 0; r < table->rowCount(); ++r) {
    auto *name = table->item(r, ui::DownloadsTab::Name);
    if (name && name->toolTip().contains(archive))
      return r;
  }
  return -1;
}

std::string cell_text(QTableWidget *table, int row, int col) {
  auto *item = table->item(row, col);
  return item ? item->text().toStdString() : std::string("<no cell>");
}

// The install the user actually performs: double-click "Install" on a
// DOWNLOADS row. That is the path the real log records
// ("Installing mod: 59818-769017" - the row key, not a mod folder), and it is
// the only thing the test here exercises. Driving the real
// PipelineWorker::install_complete signal, not DownloadsTab::mark_installed
// directly, is the point: the controller lambda is where the row is resolved
// and where a fix that only holds when called by hand dies.
TEST_CASE(
    "MainWindow: an install started from a Downloads row marks THAT row Installed",
    "[ui][harness][downloads][install]") {
  const fs::path root     = make_case_root("gmm_dl_row_from_tab");
  const fs::path inst_dir = root / "instances";

  qputenv("QT_QPA_PLATFORM", "offscreen");
  int app_argc     = 1;
  char app_argv0[] = "main_window_harness_test";
  char *app_argv[] = {app_argv0, nullptr};
  QApplication app(app_argc, app_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  // The setting the user has ON: "Hide installed". Left as-is on purpose - the
  // assertion below is made against the cell TEXT and against which rows are
  // left on screen, so a pass cannot come from the row simply being hidden.
  Settings::instance().set_hide_installed_downloads(false);

  auto inst           = engine::Instance::installed("TestGame", inst_dir);
  inst.info().game_id = "testgame";
  REQUIRE(inst.create_directories());
  REQUIRE(inst.write_toml());
  const fs::path inst_root = inst.info().root;
  const auto instance      = engine::Instance::from_root(inst_root);
  const fs::path mods_dir  = instance.path_for(engine::InstanceKind::Mods);
  const fs::path dl_dir    = instance.path_for(engine::InstanceKind::Downloads);
  fs::create_directories(dl_dir);
  fs::create_directories(mods_dir / "CrashLogger");
  write_file(mods_dir / "CrashLogger" / "CrashLogger.dll", "plugin");
  write_file(mods_dir / "CrashLogger" / "meta.ini",
             "[General]\ninstallationFile=CrashLogger-59818-1-24-0-1782605772.7z\n"
             "[GameModManager]\nfolder=CrashLogger\npriority=17\n");

  // Two downloads of the SAME mod, exactly as the user's downloads folder
  // holds them: v1.24.0 and v1.25.0, both displayed as "CrashLogger". The
  // install overwrites the mod folder, so the row the user clicked must be the
  // one that changes - the other one is still an un-installed download.
  struct Row {
    const char *archive;
    const char *id;
    int file_id;
  };
  const Row rows[] = {
      {"CrashLogger-59818-1-24-0-1782605772.7z", "59818-769017", 769017},
      {"CrashLogger 59818 1.25.0 2026-08-21T09-21Z yIjvuh041.7z", "59818-792670",
       792670},
  };
  for (const auto &r : rows) {
    const fs::path archive = dl_dir / r.archive;
    write_file(archive, "a real archive would be here");
    ui::DownloadMeta meta;
    meta.archive       = archive;
    meta.id            = r.id;
    meta.name          = "CrashLogger";
    meta.source        = "Nexus Mods";
    meta.parent_mod_id = "59818";
    meta.file_id       = r.file_id;
    meta.nexus_domain  = "testgame";
    meta.total_size    = static_cast<int64_t>(std::filesystem::file_size(archive));
    meta.state         = ui::DownloadState::Complete;
    REQUIRE(ui::write_download_meta(meta, ui::download_meta_path(meta, dl_dir)));
  }

  engine::GameKnowledge knowledge;
  knowledge.set("testgame", "mods_subpath", "Mods");

  ui::MainWindow w;
  w.set_game_knowledge(&knowledge);
  w.show();
  w.set_game_info("testgame", "Test Game", "Default", {}, inst_root);
  REQUIRE(pump_until([&w] {
    return !w.is_loading();
  }));

  auto *panel = w.findChild<ui::RightPanel *>();
  REQUIRE(panel != nullptr);
  auto *tab = panel->ensure_downloads_tab();
  REQUIRE(tab != nullptr);
  auto *table = tab->table();
  REQUIRE(table != nullptr);

  const int clicked =
      find_download_row_for_archive(table, QStringLiteral("CrashLogger-59818-1-24-0"));
  const int sibling =
      find_download_row_for_archive(table, QStringLiteral("CrashLogger 59818 1.25.0"));
  REQUIRE(clicked >= 0);
  REQUIRE(sibling >= 0);
  REQUIRE(clicked != sibling);
  REQUIRE(cell_text(table, clicked, ui::DownloadsTab::Status) == "Install");
  REQUIRE(cell_text(table, sibling, ui::DownloadsTab::Status) == "Install");

  auto *worker = w.pipeline_worker();
  REQUIRE(worker != nullptr);

  // The exact payload the real worker emits on success: the row key as the id,
  // and the archive it installed as the fallback lookup.
  Q_EMIT worker->install_complete("59818-769017", true, "Success", "CrashLogger",
                                  "CrashLogger-59818-1-24-0-1782605772.7z");
  REQUIRE(pump_until(
      [&] {
        return cell_text(table, clicked, ui::DownloadsTab::Status) != "Install";
      },
      5000));

  // THE assertion, on the row the user clicked: it reads Installed.
  CHECK(cell_text(table, clicked, ui::DownloadsTab::Status) == "Installed");
  // And the sibling, a different file that is genuinely not installed, is left
  // alone - a mark that moved to the wrong row would show up here.
  CHECK(cell_text(table, sibling, ui::DownloadsTab::Status) == "Install");

  fs::remove_all(root);
}

// The same install with the setting the user actually has ON
// (downloads/hide_installed=true). That box IS MO2's "Hide downloads after
// installation" (settingsdialog.ui:157-168, "Automatically hide downloads
// after successful installation"), and MO2's install path removes the row in
// the same call that records the install (organizercore.cpp:910-913). So a
// ticked box takes the row away the moment it says Installed - no notice, no
// grace period, no second click needed.
//
// The invented behaviour this replaces: a just-installed row was held on screen
// past the filter ("installed_notice"), on the argument that hiding it in the
// same call that recorded the install destroys the only evidence the install
// happened. It had no MO2 precedent, and the evidence argument does not hold
// when the row is the ONLY row: with the filter ticked and no sibling, an
// empty table after a successful install reads as "nothing happened" too. The
// user's own ruling is that a ticked checkbox hides Installed rows
// immediately, and MO2's setting has exactly that semantics.
TEST_CASE("MainWindow: a ticked 'Hide installed' takes the installed row at once",
          "[ui][harness][downloads][install]") {
  const fs::path root     = make_case_root("gmm_dl_row_hidden");
  const fs::path inst_dir = root / "instances";

  qputenv("QT_QPA_PLATFORM", "offscreen");
  int app_argc     = 1;
  char app_argv0[] = "main_window_harness_test";
  char *app_argv[] = {app_argv0, nullptr};
  QApplication app(app_argc, app_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  Settings::instance().set_hide_installed_downloads(true);

  auto inst           = engine::Instance::installed("TestGame", inst_dir);
  inst.info().game_id = "testgame";
  REQUIRE(inst.create_directories());
  REQUIRE(inst.write_toml());
  const fs::path inst_root = inst.info().root;
  const auto instance      = engine::Instance::from_root(inst_root);
  const fs::path mods_dir  = instance.path_for(engine::InstanceKind::Mods);
  const fs::path dl_dir    = instance.path_for(engine::InstanceKind::Downloads);
  fs::create_directories(dl_dir);
  fs::create_directories(mods_dir / "CrashLogger");
  write_file(mods_dir / "CrashLogger" / "CrashLogger.dll", "plugin");
  write_file(mods_dir / "CrashLogger" / "meta.ini",
             "[General]\ninstallationFile=CrashLogger-59818-1-24-0-1782605772.7z\n"
             "[GameModManager]\nfolder=CrashLogger\npriority=17\n");

  const char *archive = "CrashLogger-59818-1-24-0-1782605772.7z";
  const fs::path ap   = dl_dir / archive;
  write_file(ap, "a real archive would be here");
  ui::DownloadMeta meta;
  meta.archive       = ap;
  meta.id            = "59818-769017";
  meta.name          = "CrashLogger";
  meta.source        = "Nexus Mods";
  meta.parent_mod_id = "59818";
  meta.file_id       = 769017;
  meta.nexus_domain  = "testgame";
  meta.total_size    = static_cast<int64_t>(std::filesystem::file_size(ap));
  meta.state         = ui::DownloadState::Complete;
  REQUIRE(ui::write_download_meta(meta, ui::download_meta_path(meta, dl_dir)));

  engine::GameKnowledge knowledge;
  knowledge.set("testgame", "mods_subpath", "Mods");

  ui::MainWindow w;
  w.set_game_knowledge(&knowledge);
  w.show();
  w.set_game_info("testgame", "Test Game", "Default", {}, inst_root);
  REQUIRE(pump_until([&w] {
    return !w.is_loading();
  }));

  auto *panel = w.findChild<ui::RightPanel *>();
  REQUIRE(panel != nullptr);
  auto *tab = panel->ensure_downloads_tab();
  REQUIRE(tab != nullptr);
  auto *table = tab->table();
  REQUIRE(table != nullptr);

  const int clicked =
      find_download_row_for_archive(table, QStringLiteral("CrashLogger-59818-1-24-0"));
  REQUIRE(clicked >= 0);
  REQUIRE_FALSE(table->isRowHidden(clicked));

  auto *worker = w.pipeline_worker();
  REQUIRE(worker != nullptr);
  Q_EMIT worker->install_complete("59818-769017", true, "Success", "CrashLogger",
                                  archive);
  QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

  // THE assertion, with the setting the user has ON: the row they installed
  // from is marked Installed and is OFF SCREEN, in the same call. The cell is
  // still readable through the table - hiding a row is not deleting it - which
  // is where the evidence went instead of onto a notice.
  CHECK(cell_text(table, clicked, ui::DownloadsTab::Status) == "Installed");
  CHECK(table->isRowHidden(clicked));

  // The panel's own re-scan fires right after an install (the manifest write
  // trips the directory watcher). Re-pointing the tab at the same directory
  // runs that same scan, and the row stays hidden - a background refresh is
  // not a user re-asserting the filter.
  tab->set_downloads_dir(dl_dir);
  REQUIRE(pump_until(
      [&] {
        return cell_text(table, clicked, ui::DownloadsTab::Status) == "Installed";
      },
      5000));
  CHECK(table->isRowHidden(clicked));

  // Unticking is how the user gets the row back, and it must bring back exactly
  // the rows the box took - the filter hides, it does not delete.
  QCheckBox *hide_box = nullptr;
  for (auto *box : tab->findChildren<QCheckBox *>()) {
    if (box->text() == QLatin1String("Hide installed"))
      hide_box = box;
  }
  REQUIRE(hide_box != nullptr);
  REQUIRE(hide_box->isChecked());
  hide_box->setChecked(false);
  CHECK_FALSE(table->isRowHidden(clicked));
  // Re-ticking puts it away again, so the box is not one-way.
  hide_box->setChecked(true);
  CHECK(table->isRowHidden(clicked));

  fs::remove_all(root);
}

// The version an install leaves blank. The archive NAME says 1.24.0 and Nexus
// says "v1.0" for that same file - they deliberately disagree, because
// "1.24.0" is a Nexus naming convention and not a contract, and a version
// read off the filename is the wrong answer dressed as a right one. The
// captured body is the real Nexus v1 files.json reply
// (tests/engine/fixtures/nexus/files_6832.json), so the version that lands is
// Nexus's own, not one the test made up.
TEST_CASE(
    "MainWindow: an install records Nexus's version for it, not the archive's name",
    "[ui][harness][install][version]") {
  const fs::path root     = make_case_root("gmm_install_version");
  const fs::path inst_dir = root / "instances";

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
  const auto instance      = engine::Instance::from_root(inst_root);
  const fs::path mods_dir  = instance.path_for(engine::InstanceKind::Mods);
  const fs::path dl_dir    = instance.path_for(engine::InstanceKind::Downloads);
  fs::create_directories(dl_dir);
  fs::create_directories(mods_dir / "CrashLogger");
  write_file(mods_dir / "CrashLogger" / "CrashLogger.dll", "plugin");

  const char *archive = "CrashLogger-59818-1-24-0-1782605772.7z";
  const fs::path ap   = dl_dir / archive;
  write_file(ap, "a real archive would be here");
  ui::DownloadMeta meta;
  meta.archive       = ap;
  meta.id            = "59818-769017";
  meta.name          = "CrashLogger";
  meta.source        = "Nexus Mods";
  meta.parent_mod_id = "59818";
  meta.file_id       = 769017;
  meta.nexus_domain  = "testgame";
  meta.total_size    = static_cast<int64_t>(std::filesystem::file_size(ap));
  meta.state         = ui::DownloadState::Complete;
  REQUIRE(ui::write_download_meta(meta, ui::download_meta_path(meta, dl_dir)));

  engine::GameKnowledge knowledge;
  knowledge.set("testgame", "mods_subpath", "Mods");

  ui::MainWindow w;
  w.set_game_knowledge(&knowledge);
  w.show();
  w.set_game_info("testgame", "Test Game", "Default", {}, inst_root);
  REQUIRE(pump_until([&w] {
    return !w.is_loading();
  }));

  auto *ctl = w.findChild<ui::DownloadsController *>();
  REQUIRE(ctl != nullptr);
  auto *panel = w.findChild<ui::RightPanel *>();
  REQUIRE(panel != nullptr);
  REQUIRE(panel->ensure_downloads_tab() != nullptr);

  // What the install leaves behind: a meta.ini with the archive and the Nexus
  // ids, and NO version key - from_default never writes one.
  auto *worker = w.pipeline_worker();
  REQUIRE(worker != nullptr);
  fs::create_directories(mods_dir / "CrashLogger");
  write_file(mods_dir / "CrashLogger" / "meta.ini",
             "[General]\ninstallationFile=CrashLogger-59818-1-24-0-1782605772.7z\n"
             "[GameModManager]\nfolder=CrashLogger\npriority=17\n"
             "[Nexusmods]\nmodid=59818\nfileid=769017\n");

  const int mod_row = find_mod_row(w.mod_view(), QStringLiteral("CrashLogger"));
  REQUIRE(mod_row >= 0);
  auto *model = w.findChild<ui::ModList *>();
  REQUIRE(model != nullptr);
  // Before: the install's own truth, which is "not stated".
  CHECK(model->mods()[mod_row].version.toStdString().empty());

  // The version Nexus answers with for file 16268, taken from the CAPTURED
  // reply rather than typed in here.
  const fs::path fixture = fs::path(__FILE__).parent_path() / ".." / "engine" /
                           "fixtures" / "nexus" / "files_6832.json";
  std::ifstream fin(fixture, std::ios::binary);
  REQUIRE(fin.good());
  const std::string captured((std::istreambuf_iterator<char>(fin)),
                             std::istreambuf_iterator<char>());
  REQUIRE(!captured.empty());
  std::vector<long long> available;
  std::string newest;
  std::string from_nexus;
  REQUIRE(engine::Source::Nexus::UpdateCheck::parse_files(captured, 16268, available,
                                                          newest, from_nexus));
  REQUIRE(from_nexus == "v1.0");

  // The one production step that turns that answer into what the user sees.
  ui::DownloadsController::apply_installed_version(&w, mods_dir, "CrashLogger",
                                                   from_nexus);
  QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

  // THE assertions: Nexus's version is in meta.ini and on the row.
  CHECK(engine::ModMeta::load(mods_dir, "CrashLogger").get("General", "version") ==
        "v1.0");
  REQUIRE(find_mod_row(w.mod_view(), QStringLiteral("CrashLogger")) == mod_row);
  CHECK(model->mods()[mod_row].version.toStdString() == "v1.0");
  // And it is NOT the version the archive name would have suggested.
  CHECK(model->mods()[mod_row].version.toStdString() != "1.24.0");

  fs::remove_all(root);
}
