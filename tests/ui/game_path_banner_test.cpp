// Offscreen GUI regression for Workspace-tnj (set_game_info guard teardown).
//
// Covers the two behaviors the teardown must deliver:
//   1. set_game_info() with an EMPTY game dir no longer disables the whole
//      UI: profiles populate, the Downloads tab wiring runs, and the "Set
//      Game Path" banner becomes visible instead of a dead window.
//   2. Instance-owned mod-list operations (separator create, rename,
//      priority sync) work without a game dir — they only need the
//      instance's mods dir.
//
// Workspace-wk8 adds: the mod list itself loads for a game-less instance —
// ModScanWorker replaces the game-dir scan with the instance mods-dir scan.
//
// Hermetic: offscreen platform, per-case unique /tmp scratch root
// (pid + sequence, cf. TempTree in vfs_path_resolver_test.cpp) plus
// throwaway XDG_CONFIG_HOME/XDG_DATA_HOME per case, an empty
// GameKnowledge seeded with just mods_subpath. No network, no plugins.
#include "engine/core/instance/instance.h"
#include "engine/game/registry/game_knowledge.h"
#include "ui/controllers/mod_list_controller.h"
#include "ui/main_window/main_window.h"
#include "ui/settings/settings.h"
#include "ui/widgets/game_path_banner.h"
#include "ui/widgets/mod_list_model.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

// Per-TEST_CASE isolated scratch root (pid + sequence suffix): ctest
// registers each TEST_CASE as a separate test and runs them as concurrent
// worker processes, so the previous fixed /tmp/gmm_tnj_teardown path let one
// process's make_instance() remove_all() wipe another process's tree
// mid-assertion or mid async ModScanWorker scan.
//
// Uniqueness contract (same as TempTree in vfs_path_resolver_test.cpp,
// PR #170): the pid disambiguates parallel worker processes (live pids never
// collide) and the atomic sequence disambiguates instances within one
// process. A stale dir with our exact name can only come from a crashed run,
// so the ctor's remove_all() is scoped to a name no live process owns. The
// dtor is explicitly noexcept: test-helper cleanup must never throw.
struct ScopedScratch {
  std::filesystem::path root;       // scratch base, unique per instance
  std::filesystem::path instances;  // <root>/instances (instance storage)
  std::filesystem::path config;     // throwaway XDG_CONFIG_HOME
  std::filesystem::path data;       // throwaway XDG_DATA_HOME

  ScopedScratch() {
    static std::atomic<unsigned> seq{0};
#if defined(_WIN32)
    const auto pid = static_cast<unsigned long>(_getpid());
#else
    const auto pid = static_cast<unsigned long>(::getpid());
#endif
    root = std::filesystem::temp_directory_path() /
           ("gmm_tnj_teardown_" + std::to_string(pid) + "_" +
            std::to_string(seq.fetch_add(1, std::memory_order_relaxed)));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);  // stale dir from a crashed run
    instances = root / "instances";
    config    = root / "config";
    data      = root / "data";
    std::filesystem::create_directories(config, ec);
    std::filesystem::create_directories(data, ec);
  }
  ~ScopedScratch() noexcept {
    std::error_code ec;  // never throw from the dtor
    std::filesystem::remove_all(root, ec);
  }
};

// Creates <instances_root>/TestGame with dirs + instance.toml (game_id set,
// game_dir deliberately empty) — the on-disk shape of a game-less instance.
std::filesystem::path make_instance(const std::filesystem::path &instances_root) {
  auto inst           = engine::Instance::installed("TestGame", instances_root);
  inst.info().game_id = "testgame";
  REQUIRE(inst.create_directories());
  REQUIRE(inst.write_toml());
  return inst.info().root;
}

}  // namespace

TEST_CASE("set_game_info with empty game dir keeps the UI alive", "[ui]") {
  ScopedScratch scratch;
  qputenv("QT_QPA_PLATFORM", "offscreen");
  qputenv("XDG_CONFIG_HOME", QByteArray(scratch.config.string().c_str()));
  qputenv("XDG_DATA_HOME", QByteArray(scratch.data.string().c_str()));
  int test_argc     = 1;
  char test_argv0[] = "test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");
  // set_game_info posts ensure_nxm_handler_default() via singleShot(0); it
  // pops a modal NXM-handler QMessageBox inside the first processEvents
  // (infinite hang offscreen). The "dont_ask" setting is its early-out -
  // same choice a user makes with "Don't show".
  Settings::instance().set_nxm_handler_check("dont_ask");

  const auto root = make_instance(scratch.instances);

  ui::MainWindow w;
  engine::GameKnowledge knowledge;
  knowledge.set("testgame", "mods_subpath", "Mods");
  w.set_game_knowledge(&knowledge);
  w.show();

  // Must not crash and must leave a usable window behind.
  w.set_game_info("testgame", "Test Game", "", {}, root);

  auto *banner = w.findChild<ui::GamePathBanner *>();
  REQUIRE(banner != nullptr);
  CHECK(banner->isVisible());

  // The Default profile was bootstrapped by refresh_profiles().
  CHECK(std::filesystem::is_directory(root / "profiles" / "Default"));

  // A later load WITH a game dir hides the banner again.
  w.set_game_info("testgame", "Test Game", "", scratch.root / "game", root);
  CHECK_FALSE(banner->isVisible());
}

TEST_CASE("instance-owned mod ops work without a game dir", "[ui]") {
  ScopedScratch scratch;
  qputenv("QT_QPA_PLATFORM", "offscreen");
  qputenv("XDG_CONFIG_HOME", QByteArray(scratch.config.string().c_str()));
  qputenv("XDG_DATA_HOME", QByteArray(scratch.data.string().c_str()));
  int test_argc     = 1;
  char test_argv0[] = "test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");
  // set_game_info posts ensure_nxm_handler_default() via singleShot(0); it
  // pops a modal NXM-handler QMessageBox inside the first processEvents
  // (infinite hang offscreen). The "dont_ask" setting is its early-out -
  // same choice a user makes with "Don't show".
  Settings::instance().set_nxm_handler_check("dont_ask");

  const auto root = make_instance(scratch.instances);
  const auto mods_dir =
      engine::Instance::from_root(root).path_for(engine::InstanceKind::Mods);

  ui::MainWindow w;
  engine::GameKnowledge knowledge;
  knowledge.set("testgame", "mods_subpath", "Mods");
  w.set_game_knowledge(&knowledge);
  w.show();
  w.set_game_info("testgame", "Test Game", "", {}, root);

  // Workspace-wk8: set_game_info now launches the instance mods-dir scan
  // even without a game dir. It runs on ModScanThread; pump events until
  // it lands - sync_priorities skips its write while loading_.
  QElapsedTimer timer;
  timer.start();
  while (w.is_loading()) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QThread::msleep(2);
    if (timer.elapsed() > 10000)
      FAIL("instance mods-dir scan never landed");
  }

  auto *ctrl = w.findChild<ui::ModListController *>();
  REQUIRE(ctrl != nullptr);
  auto *model = w.findChild<ui::ModList *>();
  REQUIRE(model != nullptr);

  // Separator creation used to be blocked by the empty-game_dir guard.
  const auto sep_id = ctrl->create_separator_named("Cool", "");
  CHECK(sep_id == QString("Cool_separator"));
  CHECK(std::filesystem::is_directory(mods_dir / "Cool_separator"));
  const auto sep_row = std::find_if(model->mods().cbegin(), model->mods().cend(),
                                    [&sep_id](const auto &m) {
                                      return m.id == sep_id;
                                    });
  REQUIRE(sep_row != model->mods().cend());
  CHECK(sep_row->is_separator);
  const auto sep_index =
      static_cast<int>(std::distance(model->mods().cbegin(), sep_row));

  // Rename moves the folder under the instance mods dir.
  ctrl->apply_rename(sep_index, "Renamed");
  CHECK(std::filesystem::is_directory(mods_dir / "Renamed_separator"));
  CHECK_FALSE(std::filesystem::exists(mods_dir / "Cool_separator"));

  // Priority sync persists to the mod's in-folder meta.ini without a game
  // dir. The write only happens when a row's priority actually changes
  // away from the default 0, so move the separator below the Overwrite row.
  model->move_mod(QString("Renamed_separator"), 1);
  ctrl->sync_priorities();
  CHECK(std::filesystem::exists(mods_dir / "Renamed_separator" / "meta.ini"));
}

// Workspace-wk8: the mod list loads for a game-less instance. The scan
// runs against the instance mods dir (ModScanWorker swaps it in when
// game_dir is empty), so a mod folder seeded there shows up.
TEST_CASE("mod list loads from instance mods dir without a game dir", "[ui]") {
  ScopedScratch scratch;
  qputenv("QT_QPA_PLATFORM", "offscreen");
  qputenv("XDG_CONFIG_HOME", QByteArray(scratch.config.string().c_str()));
  qputenv("XDG_DATA_HOME", QByteArray(scratch.data.string().c_str()));
  int test_argc     = 1;
  char test_argv0[] = "test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");
  // set_game_info posts ensure_nxm_handler_default() via singleShot(0); it
  // pops a modal NXM-handler QMessageBox inside the first processEvents
  // (infinite hang offscreen). The "dont_ask" setting is its early-out -
  // same choice a user makes with "Don't show".
  Settings::instance().set_nxm_handler_check("dont_ask");

  const auto root = make_instance(scratch.instances);
  const auto mods_dir =
      engine::Instance::from_root(root).path_for(engine::InstanceKind::Mods);

  // Seed one mod folder before the load - exactly what an install into a
  // game-less instance would leave behind.
  std::error_code ec;
  std::filesystem::create_directories(mods_dir / "Foo_mod", ec);
  REQUIRE(ec == std::error_code{});

  ui::MainWindow w;
  engine::GameKnowledge knowledge;
  knowledge.set("testgame", "mods_subpath", "Mods");
  w.set_game_knowledge(&knowledge);
  w.show();
  w.set_game_info("testgame", "Test Game", "", {}, root);

  auto *model = w.findChild<ui::ModList *>();
  REQUIRE(model != nullptr);

  // The scan is async (ModScanThread); pump events until the result
  // converges into the model.
  QElapsedTimer timer;
  timer.start();
  bool found = false;
  while (!found) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QThread::msleep(2);
    const auto &mods = model->mods();
    found            = std::any_of(mods.cbegin(), mods.cend(), [](const auto &m) {
      return m.id == "Foo_mod";
    });
    if (!found && timer.elapsed() > 10000)
      FAIL("mod scan never landed for the game-less instance");
  }
}
