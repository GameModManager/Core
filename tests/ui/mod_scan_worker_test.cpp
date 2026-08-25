// ModScanWorker stray-plugin synthesis regression test (Workspace-kjt).
//
// Pins the contract that deployed .esp files must NOT be synthesized as
// unmanaged rows: in direct-symlink mode the deploy ledger records every
// target the manager deployed into game_dir/Data, and the stray scan consults
// it before synthesizing an 'Unmanaged: <file>' row. The test covers:
//   1. A deployed plugin that is a symlink (the actual direct-mode artifact)
//      is skipped.
//   2. A deployed plugin that is a REAL file (e.g. a game overwrote the
//      symlink, or a future mode copies .esp) is skipped via the ledger check
//      alone — proving the ledger, not the symlink guard, is the source of
//      truth.
//   3. The ledger comparison survives a differently-spelled game dir: the
//      ledger stores <base>/game/Data/... targets while the scan request uses
//      <base>/gamelink (a symlink to game) — weakly_canonical on both sides
//      makes them match.
//   4. A real plugin the user dropped into Data/ with no ledger entry is
//      STILL synthesized as an unmanaged row (the MO2 UnmanagedMods behavior
//      is preserved for genuinely unmanaged files).
//
// Hermetic: QCoreApplication (no widgets), throwaway temp dir under the build
// dir, knowledge hooks drive the scan (no game plugin needed).
#include "ui/main_window/mod_scan_worker.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#include "engine/game/registry/game_knowledge.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace {
void check(bool cond, const char *what) {
  INFO(what);
  REQUIRE(cond);
}

void write_file(const fs::path &p, const std::string &contents) {
  fs::create_directories(p.parent_path());
  std::ofstream out(p);
  out << contents;
  check(out.good(),
        (std::string("write_file failed for ") + p.string()).c_str());
}

const engine::ScannedMod *by_folder(const std::vector<engine::ScannedMod> &mods,
                                    const std::string &folder) {
  for (const auto &m : mods)
    if (m.folder_name == folder)
      return &m;
  return nullptr;
}
} // namespace

TEST_CASE("mod scan worker stray plugins", "[ui]") {
  int test_argc = 1;
  char test_argv0[] = "test";
  char *test_argv[] = {test_argv0, nullptr};
  QCoreApplication app(test_argc, test_argv);
  (void)app;

  const fs::path base = fs::current_path() / ("gmm_test_mod_scan_worker_" +
                                              std::to_string(getpid()));
  const fs::path game_dir = base / "game";
  const fs::path data_dir = game_dir / "Data";
  const fs::path mods_dir = base / "mods";
  const fs::path meta_dir = base / "meta";
  const fs::path ledger_file = base / ".gmm_deploy_ledger";
  std::error_code ec;
  fs::create_directories(data_dir, ec);
  fs::create_directories(mods_dir, ec);
  fs::create_directories(meta_dir, ec);

  // A mod folder owning a plugin, as a real install would produce.
  fs::create_directories(mods_dir / "MyMod", ec);
  write_file(mods_dir / "MyMod" / "MyMod.esp", "TES4");

  // Deployed artifact #1: the real direct-symlink shape — a symlink in
  // Data/ pointing back into the mod folder.
  fs::create_symlink(mods_dir / "MyMod" / "MyMod.esp", data_dir / "MyMod.esp",
                     ec);
  check(!ec, "deployed symlink created");

  // Deployed artifact #2: a REAL file that the ledger also owns (a game
  // overwrote the symlink, or a future mode copies .esp). Only the ledger
  // check can skip this one — the symlink guard cannot.
  write_file(data_dir / "LedgerReal.esp", "TES4");

  // Genuinely unmanaged: a real plugin the user dropped in, no ledger entry.
  write_file(data_dir / "UserDrop.esp", "TES4");

  // The deploy ledger, exactly as deploy_all_enabled_mods_direct writes it
  // (target<TAB>source per line). Targets are spelled via the REAL game dir;
  // the scan request below uses a symlinked spelling to prove the
  // weakly_canonical comparison.
  {
    std::ofstream out(ledger_file);
    out << (game_dir / "Data" / "MyMod.esp").string() << '\t'
        << (mods_dir / "MyMod" / "MyMod.esp").string() << '\n';
    out << (game_dir / "Data" / "LedgerReal.esp").string() << '\t'
        << (mods_dir / "MyMod" / "MyMod.esp").string() << '\n';
    check(out.good(), "ledger written");
  }

  // Symlinked game-dir spelling: the scan request sees the game through
  // <base>/gamelink -> <base>/game, while the ledger stored <base>/game/...
  // targets. weakly_canonical must reconcile the two.
  const fs::path game_link = base / "gamelink";
  fs::create_symlink(game_dir, game_link, ec);
  check(!ec, "game-dir symlink created");

  engine::GameKnowledge knowledge;
  knowledge.set("testgame", "mods_subpath", "Data");
  knowledge.set("testgame", "game_native_plugins", "Skyrim.esm");

  ui::ModScanRequest req;
  req.knowledge = knowledge;
  req.game_id = "testgame";
  req.game_dir = game_link; // symlinked spelling
  req.instance_root = base;
  req.mods_dir = mods_dir;
  req.meta_dir = meta_dir;
  req.ledger_file = ledger_file;

  struct ScanResult {
    ui::ModScanResult result;
    quint64 generation = 0;
  };
  std::vector<ScanResult> results;
  ui::ModScanThread thread(&app);
  ui::ModScanWorker *worker = thread.worker();
  QObject::connect(worker, &ui::ModScanWorker::finished, &app,
                   [&](ui::ModScanResult result, quint64 generation) {
                     results.push_back({std::move(result), generation});
                   });

  thread.start(std::move(req), /*generation=*/1);

  QElapsedTimer timer;
  timer.start();
  while (results.empty()) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QThread::msleep(2);
    if (timer.elapsed() > 10000) {
      FAIL("scan never landed");
    }
  }

  check(results.size() == 1, "exactly one result for the single scan");
  check(results[0].generation == 1, "finished() carries the run's generation");

  const auto &scanned = results[0].result.scanned;

  // The mod folder itself is scanned from the instance mods dir.
  check(by_folder(scanned, "MyMod") != nullptr,
        "mod folder scanned from the instance mods dir");

  // Deployed plugins (symlink AND real-file ledger entries) are NOT
  // synthesized as unmanaged rows.
  check(by_folder(scanned, "MyMod.esp") == nullptr,
        "deployed symlink .esp is not synthesized as an unmanaged row");
  check(by_folder(scanned, "LedgerReal.esp") == nullptr,
        "ledger-owned real .esp is not synthesized as an unmanaged row");

  // A genuinely unmanaged plugin still gets the MO2 UnmanagedMods row.
  const auto *user_drop = by_folder(scanned, "UserDrop.esp");
  check(user_drop != nullptr, "user-dropped .esp still synthesized");
  check(user_drop != nullptr && user_drop->is_game_native,
        "user-dropped .esp row is flagged game-native (renders Unmanaged)");

  fs::remove_all(base, ec);
}

// Workspace-6up: the ModScanRequest.game_mods_dir override redirects the
// game-native mods dir (stray-plugin synthesis) away from
// game_dir/mods_subpath — the Isaac-on-macOS shape where the real mods
// folder lives outside the install dir.
TEST_CASE("mod scan worker game mods dir override", "[ui]") {
  int test_argc = 1;
  char test_argv0[] = "test";
  char *test_argv[] = {test_argv0, nullptr};
  QCoreApplication app(test_argc, test_argv);
  (void)app;

  const fs::path base = fs::current_path() / ("gmm_test_mod_scan_worker_ov_" +
                                              std::to_string(getpid()));
  const fs::path game_dir = base / "game";
  const fs::path data_dir = game_dir / "Data";
  // The actual mods folder, OUTSIDE the game dir.
  const fs::path external_mods =
      base / "Binding of Isaac Afterbirth+ Mods";
  const fs::path mods_dir = base / "mods";
  const fs::path meta_dir = base / "meta";
  std::error_code ec;
  fs::create_directories(data_dir, ec);
  fs::create_directories(external_mods, ec);
  fs::create_directories(mods_dir, ec);
  fs::create_directories(meta_dir, ec);

  // A stray plugin in the EXTERNAL mods folder only.
  write_file(external_mods / "ExternalStray.esp", "TES4");
  // And one in the classic Data dir that must NOT be picked up while the
  // override points elsewhere.
  write_file(data_dir / "DataStray.esp", "TES4");

  engine::GameKnowledge knowledge;
  knowledge.set("testgame", "mods_subpath", "Data");
  knowledge.set("testgame", "game_native_plugins", "Skyrim.esm");

  ui::ModScanRequest req;
  req.knowledge = knowledge;
  req.game_id = "testgame";
  req.game_dir = game_dir;
  req.game_mods_dir = external_mods; // the override under test
  req.instance_root = base;
  req.mods_dir = mods_dir;
  req.meta_dir = meta_dir;

  struct ScanResult {
    ui::ModScanResult result;
    quint64 generation = 0;
  };
  std::vector<ScanResult> results;
  ui::ModScanThread thread(&app);
  ui::ModScanWorker *worker = thread.worker();
  QObject::connect(worker, &ui::ModScanWorker::finished, &app,
                   [&](ui::ModScanResult result, quint64 generation) {
                     results.push_back({std::move(result), generation});
                   });

  thread.start(std::move(req), /*generation=*/1);

  QElapsedTimer timer;
  timer.start();
  while (results.empty()) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QThread::msleep(2);
    if (timer.elapsed() > 10000) {
      FAIL("scan never landed");
    }
  }

  const auto &scanned = results[0].result.scanned;
  check(by_folder(scanned, "ExternalStray.esp") != nullptr,
        "stray plugin in the overridden mods dir is synthesized");
  check(by_folder(scanned, "DataStray.esp") == nullptr,
        "stray plugin in the old game_dir/Data location is ignored");

  fs::remove_all(base, ec);
}

// Workspace-8j8: when game_mods_dir is set, the game-dir scan found REAL
// deployed mods and must NOT be replaced by the instance mods-dir scan.
// The instance mods dir (GMM storage for downloaded-but-not-yet-deployed
// mods) merges IN, deduped by folder name.
TEST_CASE("mod scan worker merge keeps game mods dir results", "[ui]") {
  int test_argc = 1;
  char test_argv0[] = "test";
  char *test_argv[] = {test_argv0, nullptr};
  QCoreApplication app(test_argc, test_argv);
  (void)app;

  const fs::path base = fs::current_path() / ("gmm_test_mod_scan_worker_mg_" +
                                              std::to_string(getpid()));
  const fs::path game_dir = base / "game";
  const fs::path data_dir = game_dir / "Data";
  // The actual mods folder, OUTSIDE the game dir (Isaac-on-macOS shape).
  const fs::path external_mods = base / "Isaac Mods";
  // GMM's own storage inside the instance root.
  const fs::path mods_dir = base / "mods";
  std::error_code ec;
  fs::create_directories(data_dir, ec);
  fs::create_directories(external_mods, ec);
  fs::create_directories(mods_dir, ec);

  // Real deployed mods in the game's mods folder...
  fs::create_directories(external_mods / "DeployedA", ec);
  write_file(external_mods / "DeployedA" / "mod.json", "{}");
  fs::create_directories(external_mods / "DeployedB", ec);
  write_file(external_mods / "DeployedB" / "mod.json", "{}");
  // ...plus one whose name also exists in the instance mods dir (the
  // downloaded copy of the same mod) - must appear exactly once.
  fs::create_directories(external_mods / "Shared", ec);
  write_file(external_mods / "Shared" / "mod.json", "{}");
  // A mod stored by GMM but not yet deployed.
  fs::create_directories(mods_dir / "StoredOnly", ec);
  write_file(mods_dir / "StoredOnly" / "mod.json", "{}");
  fs::create_directories(mods_dir / "Shared", ec);
  write_file(mods_dir / "Shared" / "mod.json", "{}");

  engine::GameKnowledge knowledge;
  knowledge.set("testgame", "mods_subpath", "Data");

  ui::ModScanRequest req;
  req.knowledge = knowledge;
  req.game_id = "testgame";
  req.game_dir = game_dir;
  req.game_mods_dir = external_mods; // set => results are real, keep them
  req.instance_root = base;
  req.mods_dir = mods_dir;

  struct ScanResult {
    ui::ModScanResult result;
    quint64 generation = 0;
  };
  std::vector<ScanResult> results;
  ui::ModScanThread thread(&app);
  ui::ModScanWorker *worker = thread.worker();
  QObject::connect(worker, &ui::ModScanWorker::finished, &app,
                   [&](ui::ModScanResult result, quint64 generation) {
                     results.push_back({std::move(result), generation});
                   });

  thread.start(std::move(req), /*generation=*/1);

  QElapsedTimer timer;
  timer.start();
  while (results.empty()) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QThread::msleep(2);
    if (timer.elapsed() > 10000) {
      FAIL("scan never landed");
    }
  }

  const auto &scanned = results[0].result.scanned;
  check(by_folder(scanned, "DeployedA") != nullptr,
        "game-mods-dir mod DeployedA survives the instance-mode scan");
  check(by_folder(scanned, "DeployedB") != nullptr,
        "game-mods-dir mod DeployedB survives the instance-mode scan");
  check(by_folder(scanned, "StoredOnly") != nullptr,
        "instance-stored mod is merged in");
  int shared_count = 0;
  for (const auto &m : scanned)
    if (m.folder_name == "Shared")
      ++shared_count;
  check(shared_count == 1, "folder present in both dirs appears exactly once");

  fs::remove_all(base, ec);
}