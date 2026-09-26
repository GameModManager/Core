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
#include "engine/sort/sorter/interface.h"
#include "engine/sort/sorter/registry.h"
#include "ui/controllers/mod_list_controller.h"
#include "ui/main_window/main_window.h"
#include "ui/settings/settings.h"
#include "ui/widgets/game_path_banner.h"
#include "ui/widgets/mod_list_model.h"

#include <QApplication>
#include <QByteArray>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

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

// Per-case Qt + isolation setup (was triplicated verbatim in each TEST_CASE):
// unique scratch root, offscreen/XDG env before QApplication, org/app names,
// and the dont_ask seed. Same statements in the same order - just DRY.
// Env goes before the QApplication emplace (it is read at construction);
// org/app + dont_ask go after (QCoreApplication must exist, and dont_ask is
// the early-out for ensure_nxm_handler_default()'s modal NXM-handler box,
// which would hang forever offscreen on the first processEvents pump).
struct CaseSetup {
  ScopedScratch scratch;
  int test_argc      = 1;
  char test_argv0[5] = "test";
  char *test_argv[2] = {test_argv0, nullptr};
  std::optional<QApplication> app;

  CaseSetup() {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("XDG_CONFIG_HOME", QByteArray(scratch.config.string().c_str()));
    qputenv("XDG_DATA_HOME", QByteArray(scratch.data.string().c_str()));
    app.emplace(test_argc, test_argv);
    QCoreApplication::setOrganizationName("GameModManager");
    QCoreApplication::setApplicationName("GameModManager");
    // set_game_info posts ensure_nxm_handler_default() via singleShot(0); it
    // pops a modal NXM-handler QMessageBox inside the first processEvents
    // (infinite hang offscreen). The "dont_ask" setting is its early-out -
    // same choice a user makes with "Don't show".
    Settings::instance().set_nxm_handler_check("dont_ask");
  }
};

// Workspace-8tqw: the game hooks TheBindingOfIsaacRebirth registers for
// sort-order persistence. Seeded under a local game id so the case needs no
// plugin install (TheBindingOfIsaacRebirth.cpp:68-74 / :300-321).
void seed_game_native_order_knowledge(engine::GameKnowledge &knowledge,
                                      const std::string &game_id) {
  knowledge.set(game_id, "mods_subpath", "mods");
  knowledge.set(game_id, "game_mods_dir", "mods");
  knowledge.set(game_id, "metadata_file", "metadata.xml");
  knowledge.set(game_id, "metadata_name_tag", "name");
  knowledge.set(game_id, "priority_prefix_re", "^[^a-zA-Z]+");
  knowledge.set(game_id, "priority_format", "%03d ");
}

// Blocking-free wait for the async ModScanThread result: pump events until
// `done` holds, fail the case if the scan never lands.
template <typename Done>
void pump_until(Done done) {
  QElapsedTimer timer;
  timer.start();
  while (!done()) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QThread::msleep(2);
    if (timer.elapsed() > 10000)
      FAIL("mod scan never landed");
  }
}

std::string read_text(const std::filesystem::path &p) {
  std::ifstream f(p);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

void write_text(const std::filesystem::path &p, const std::string &body) {
  std::filesystem::create_directories(p.parent_path());
  std::ofstream out(p);
  out << body;
  REQUIRE(out.good());
}

// A Sorter::Registry provider that reverses the model's order, so a persisted
// sort can never coincide with the seeded one.
class ReverseSorter : public engine::Sorter::Interface {
public:
  engine::Sorter::Result
  sort(const std::vector<engine::Sorter::ModInfo> &mods) const override {
    engine::Sorter::Result out;
    for (auto it = mods.rbegin(); it != mods.rend(); ++it)
      out.sorted_folders.push_back(it->folder_name);
    return out;
  }
  const char *name() const override { return "reverse"; }
};

}  // namespace

TEST_CASE("set_game_info with empty game dir keeps the UI alive", "[ui]") {
  CaseSetup setup;

  const auto root = make_instance(setup.scratch.instances);

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
  w.set_game_info("testgame", "Test Game", "", setup.scratch.root / "game", root);
  CHECK_FALSE(banner->isVisible());
}

TEST_CASE("instance-owned mod ops work without a game dir", "[ui]") {
  CaseSetup setup;

  const auto root = make_instance(setup.scratch.instances);
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
  CaseSetup setup;

  const auto root = make_instance(setup.scratch.instances);
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

// Workspace-8tqw: the game-native sort-order write must follow the row's
// content_dir (<game_dir>/mods/<id>), not the instance mods dir. Two on-disk
// shapes, both produced by real code paths:
//   Stub_mod   - instance/mods/<id> is a meta.ini-only stub while the real
//                mod lives in <game_dir>/mods/<id> (the merge in
//                ModScanWorker:136-160). Writing at the instance folder is a
//                silent no-op: there is no metadata.xml there.
//   Mirror_mod - instance/mods/<id> is a full backup copy INCLUDING
//                metadata.xml (Workspace-0pi5). Writing at the instance
//                folder "succeeds" but lands in the backup, which the game
//                never reads.
TEST_CASE("Isaac sort order is written to the game mods dir", "[ui]") {
  CaseSetup setup;

  const auto root = make_instance(setup.scratch.instances);
  const auto mods_dir =
      engine::Instance::from_root(root).path_for(engine::InstanceKind::Mods);
  const auto game_dir  = setup.scratch.root / "game";
  const auto game_mods = game_dir / "mods";

  const std::string stub_meta = "<mod><name>Stub</name><version>1.0</version></mod>";
  const std::string mirror_meta =
      "<mod><name>Mirror</name><version>1.0</version></mod>";

  // MERGED shape: instance stub only.
  write_text(mods_dir / "Stub_mod" / "meta.ini", "[General]\nversion = 1.0\n");
  // MIRRORED shape: the instance folder is a full copy, metadata included.
  write_text(mods_dir / "Mirror_mod" / "meta.ini", "[General]\nversion = 1.0\n");
  write_text(mods_dir / "Mirror_mod" / "metadata.xml", mirror_meta);
  // The real source in the game mods dir, for both.
  write_text(game_mods / "Stub_mod" / "metadata.xml", stub_meta);
  write_text(game_mods / "Mirror_mod" / "metadata.xml", mirror_meta);

  ui::MainWindow w;
  engine::GameKnowledge knowledge;
  seed_game_native_order_knowledge(knowledge, "testgame");
  w.set_game_knowledge(&knowledge);
  w.show();
  w.set_game_info("testgame", "Test Game", "", game_dir, root);

  auto *model = w.findChild<ui::ModList *>();
  REQUIRE(model != nullptr);
  auto *ctrl = w.findChild<ui::ModListController *>();
  REQUIRE(ctrl != nullptr);

  // Wait for the merged scan: both rows must carry the game-side content_dir,
  // which is what sync_priorities has to follow.
  pump_until([&] {
    int seen = 0;
    for (const auto &m : model->mods()) {
      if ((m.id == "Stub_mod" || m.id == "Mirror_mod") && !m.content_dir.isEmpty() &&
          std::filesystem::exists(std::filesystem::path(m.content_dir.toStdString())))
        ++seen;
    }
    return seen == 2;
  });

  // Reorder so a row moves away from the priority it was seeded at. scan_dir
  // sorts by display_name, so don't hardcode which row sits where: move
  // Mirror_mod onto Stub_mod's index, which swaps the pair for any scan order
  // instead of landing on its own index (a no-op move).
  const auto stub_row = std::find_if(model->mods().cbegin(), model->mods().cend(),
                                     [](const auto &m) { return m.id == "Stub_mod"; });
  REQUIRE(stub_row != model->mods().cend());
  const auto stub_index =
      static_cast<int>(std::distance(model->mods().cbegin(), stub_row));
  model->move_mod(QString("Mirror_mod"), stub_index);
  // The move has to be a real reorder: a self-move would leave both rows on
  // their seeded priority and make every assertion below vacuous.
  REQUIRE(model->mods()[stub_index].id == "Mirror_mod");

  ctrl->sync_priorities();

  // Expected prefix per row = its index after the reorder (what
  // sync_priorities passes to set_priority).
  const auto expected_prefix = [&](const QString &id) {
    const auto it =
        std::find_if(model->mods().cbegin(), model->mods().cend(), [&](const auto &m) {
          return m.id == id;
        });
    REQUIRE(it != model->mods().cend());
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%03d ",
                  static_cast<int>(std::distance(model->mods().cbegin(), it)));
    return std::string(buf);
  };

  const auto stub_xml = read_text(game_mods / "Stub_mod" / "metadata.xml");
  INFO("game-side Stub_mod metadata.xml was: " << stub_xml);
  CHECK(stub_xml.find("<name>" + expected_prefix("Stub_mod") + "Stub<") !=
        std::string::npos);

  const auto mirror_xml = read_text(game_mods / "Mirror_mod" / "metadata.xml");
  INFO("game-side Mirror_mod metadata.xml was: " << mirror_xml);
  CHECK(mirror_xml.find("<name>" + expected_prefix("Mirror_mod") + "Mirror<") !=
        std::string::npos);

  // The instance backup copy is not what the game reads - it must stay byte
  // identical, i.e. the write must not have landed there.
  CHECK(read_text(mods_dir / "Mirror_mod" / "metadata.xml") == mirror_meta);
}

// Workspace-8tqw: sort_mods() used to rebuild the model while w_->loading_
// was still true, so the mod_list_changed receiver's sync_priorities()
// early-returned and the sort persisted NOTHING (not the in-folder meta.ini,
// not the game-native metadata). Applies to every Sorter::Registry game, not
// just Isaac.
TEST_CASE("sort_mods persists the new order to disk", "[ui]") {
  CaseSetup setup;

  const auto root = make_instance(setup.scratch.instances);
  const auto mods_dir =
      engine::Instance::from_root(root).path_for(engine::InstanceKind::Mods);
  for (const auto *id : {"A_mod", "B_mod", "C_mod"})
    write_text(mods_dir / id / "meta.ini", "[General]\nversion = 1.0\n");

  engine::Sorter::Registry::instance().register_provider(
      "testgame", std::make_unique<ReverseSorter>());

  ui::MainWindow w;
  engine::GameKnowledge knowledge;
  knowledge.set("testgame", "mods_subpath", "Mods");
  w.set_game_knowledge(&knowledge);
  w.show();
  w.set_game_info("testgame", "Test Game", "", {}, root);

  auto *model = w.findChild<ui::ModList *>();
  REQUIRE(model != nullptr);
  auto *ctrl = w.findChild<ui::ModListController *>();
  REQUIRE(ctrl != nullptr);

  pump_until([&] {
    int seen = 0;
    for (const auto &m : model->mods())
      if (m.id == "A_mod" || m.id == "B_mod" || m.id == "C_mod")
        ++seen;
    return seen == 3;
  });

  ctrl->sort_mods();

  // The provider reversed the order, so every row's index differs from the
  // seed order - each mod's meta.ini priority must now carry its row index.
  for (int i = 0; i < model->mods().size(); ++i) {
    const auto &m = model->mods()[i];
    if (m.id != "A_mod" && m.id != "B_mod" && m.id != "C_mod")
      continue;
    const auto meta = read_text(mods_dir / m.id.toStdString() / "meta.ini");
    INFO(m.id.toStdString() << " meta.ini was: " << meta);
    CHECK(meta.find("priority = " + std::to_string(i)) != std::string::npos);
  }
  // Sanity: the reversal really happened, otherwise the case is vacuous.
  CHECK(model->mods().front().id == "C_mod");

  engine::Sorter::Registry::instance().clear();
}
