// Data tab vs instance switch (Workspace-0sp3).
//
// The Data tab is the one right-panel tab that survives an instance switch
// (the lazy tabs are deleted and rebuilt by RightPanel::set_game). Nothing
// used to clear it: the previous instance's rows stayed visible until the
// new instance's first conflict scan landed - and forever for a game-less
// instance, whose recompute_conflicts() early-return never refreshes the
// tab. set_game_info now clears the tab (and the previous instance's
// conflict registry) synchronously at switch time; clear_content() bumps
// the tab's build generation, so an in-flight row build for the old
// instance is discarded when it lands.
//
// Drives the REAL MainWindow + set_game_info (the production switch path:
// mod scan -> conflict scan -> refresh_data_tab -> worker build), asserts
// the tree is empty the instant the switch returns, then that the new
// instance's rows arrive and the old instance's never come back.
//
// Hermetic: offscreen platform, throwaway /tmp root (XDG_CONFIG_HOME +
// XDG_DATA_HOME + HOME), no network, no real game install.
#include "engine/instance/instance.h"
#include "engine/game/registry/game_knowledge.h"
#include "platform/platform.h"
#include "test_platform.h"
#include "ui/main_window/main_window.h"
#include "ui/panels/data_tab.h"
#include "ui/settings/settings.h"
#include "ui/widgets/mod_list_model.h"
#include "ui/widgets/right_panel.h"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

void write_file(const fs::path &p, const char *content) {
  fs::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary);
  out << content;
}

// Pump the main thread until cond() holds or the deadline expires. The mod
// scan, conflict scan and Data tab row build all run on worker threads and
// land via queued signals, so the test must spin the event loop.
template <typename Pred>
bool pump_until(Pred cond, int timeout_ms = 20000) {
  QElapsedTimer timer;
  timer.start();
  while (!cond()) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QThread::msleep(5);
    if (timer.elapsed() > timeout_ms)
      return false;
  }
  QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  return true;
}

QTreeWidgetItem *find_top_row(QTreeWidget *tree, const QString &name) {
  auto *root = tree->invisibleRootItem();
  for (int i = 0; i < root->childCount(); ++i) {
    if (root->child(i)->text(0) == name)
      return root->child(i);
  }
  return nullptr;
}

// Per-case throwaway root: config/data/instances live inside it, so
// Settings (XDG_CONFIG_HOME) and the trash (XDG_DATA_HOME) are isolated
// from the real user config.
fs::path make_case_root(const char *name) {
  const fs::path root = fs::path("/tmp") / name;
  fs::remove_all(root);
  fs::create_directories(root / "config");
  fs::create_directories(root / "data");
  fs::create_directories(root / "instances");
  qputenv("XDG_CONFIG_HOME", QByteArray((root / "config").string().c_str()));
  qputenv("XDG_DATA_HOME", QByteArray((root / "data").string().c_str()));
  qputenv("HOME", QByteArray(root.string().c_str()));
  // set_game_info posts ensure_nxm_handler_default() via singleShot(0);
  // the "dont_ask" setting is its early-out (main_window_harness_test).
  Settings::instance().set_nxm_handler_check("dont_ask");
  return root;
}

// Platform stub with throwaway dirs, the main_window_harness_test shape.
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

// One instance with one enabled mod carrying one data file, so the real
// scan chain (mod scan -> conflict scan -> Data tab build) has a row to show.
struct SwitchFixture {
  fs::path root;
  fs::path game_dir;
  fs::path instance_root;
  const char *row_name = nullptr;
};

SwitchFixture make_instance(const fs::path &cases_root, const std::string &name,
                            const char *file_name) {
  SwitchFixture f;
  f.root     = cases_root / name;
  f.game_dir = f.root / "game";
  write_file(f.game_dir / "Data" / "Skyrim.esm", "fake esm");

  auto inst           = engine::Instance::installed(name, cases_root / "instances");
  inst.info().game_id = "testgame";
  REQUIRE(inst.create_directories());
  REQUIRE(inst.write_toml());
  f.instance_root = inst.info().root;

  const auto inst_view    = engine::Instance::from_root(f.instance_root);
  const fs::path mods_dir = inst_view.path_for(engine::InstanceKind::Mods);
  write_file(mods_dir / "Mod" / "meta.ini", "[General]\npriority=0\n");
  write_file(mods_dir / "Mod" / file_name, "content");
  // A mod not listed in the profile's modlist.txt is appended DISABLED
  // (MO2's refreshModStatus behavior), so list it enabled up front.
  const fs::path profiles_dir = inst_view.path_for(engine::InstanceKind::Profiles);
  write_file(profiles_dir / "Default" / "modlist.txt", "+Mod\n");
  f.row_name = file_name;
  return f;
}

}  // namespace

TEST_CASE("Data tab clears on instance switch and shows only the new instance",
          "[ui]") {
  const fs::path cases_root = make_case_root("gmm_data_switch");
  const SwitchFixture a     = make_instance(cases_root, "instA", "a_file.txt");
  const SwitchFixture b     = make_instance(cases_root, "instB", "b_file.txt");

  qputenv("QT_QPA_PLATFORM", "offscreen");
  int app_argc     = 1;
  char app_argv0[] = "data_tab_instance_switch_test";
  char *app_argv[] = {app_argv0, nullptr};
  QApplication app(app_argc, app_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  engine::GameKnowledge knowledge;
  knowledge.set("testgame", "mods_subpath", "Mods");

  FakePlatform platform(cases_root / "data");

  ui::MainWindow w;
  w.set_game_knowledge(&knowledge);
  w.set_platform(&platform);
  w.show();

  // Load instance A and wait for the real scan chain to populate the tab.
  w.set_game_info("testgame", "Test Game", "Default", a.game_dir, a.instance_root);
  auto *rp = w.findChild<ui::RightPanel *>();
  REQUIRE(rp != nullptr);
  auto *tree = rp->data_tab()->tree();
  REQUIRE(tree != nullptr);
  REQUIRE(pump_until([&w] {
    return !w.is_loading();
  }));
  REQUIRE(pump_until([&] {
    return find_top_row(tree, a.row_name) != nullptr;
  }));

  // Switch to instance B. set_game_info is synchronous and nothing has been
  // pumped since: the tab must be empty the instant the switch returns,
  // before B's scan chain has had a single event-loop turn.
  w.set_game_info("testgame", "Test Game", "Default", b.game_dir, b.instance_root);
  INFO("Data tab still shows instance A's row after the switch");
  CHECK(find_top_row(tree, a.row_name) == nullptr);
  CHECK(tree->topLevelItemCount() == 0);

  // B's rows arrive through the real scan chain; A's must never come back
  // (a late build for the old instance lands with a stale generation).
  REQUIRE(pump_until([&] {
    return find_top_row(tree, b.row_name) != nullptr;
  }));
  CHECK(find_top_row(tree, a.row_name) == nullptr);

  // Stability: extra event-loop turns must not resurrect A's rows.
  for (int i = 0; i < 20; ++i) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    QThread::msleep(5);
  }
  CHECK(find_top_row(tree, a.row_name) == nullptr);
  CHECK(find_top_row(tree, b.row_name) != nullptr);

  fs::remove_all(cases_root);
}
