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

#include "engine/mod/meta/mod_meta.h"

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
  const fs::path ledger_file = base / ".gmm_deploy_ledger";
  std::error_code ec;
  fs::create_directories(data_dir, ec);
  fs::create_directories(mods_dir, ec);

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
  std::error_code ec;
  fs::create_directories(data_dir, ec);
  fs::create_directories(external_mods, ec);
  fs::create_directories(mods_dir, ec);

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

// Workspace-pmrh: legacy sidecars ({root}/meta/*.ini) migrate into the
// MO2-compatible in-folder location (mods/{folder}/meta.ini) on scan:
// sidecar-only moves, both-present merges (sidecar wins manager keys, the
// folder wins game keys), orphans move to meta.bak/, and raw MO2 metas are
// enriched in place. Idempotent: a second scan changes nothing.
TEST_CASE("mod scan worker migrates sidecars in-folder", "[ui]") {
  int test_argc = 1;
  char test_argv0[] = "test";
  char *test_argv[] = {test_argv0, nullptr};
  QCoreApplication app(test_argc, test_argv);
  (void)app;

  const fs::path base = fs::current_path() / ("gmm_test_mod_scan_migrate_" +
                                              std::to_string(getpid()));
  const fs::path mods_dir = base / "mods";
  const fs::path legacy_dir = base / "meta";
  std::error_code ec;
  fs::create_directories(mods_dir / "ModA", ec);
  fs::create_directories(mods_dir / "ModB", ec);
  fs::create_directories(mods_dir / "ModC", ec);
  fs::create_directories(legacy_dir, ec);

  // ModA: BOTH exist - in-folder holds game data, sidecar holds manager state.
  write_file(mods_dir / "ModA" / "meta.ini",
             "[General]\nversion=9.9\n\n[GameModManager]\nfolder=ModA\n");
  write_file(legacy_dir / "ModA.ini",
             "[GameModManager]\nfolder=ModA\npriority=5\nsource_type=nexus\n"
             "source_id=42\n\n[General]\ncategory=7\n");
  // ModB: sidecar only (the XML-metadata-game shape).
  write_file(legacy_dir / "ModB.ini",
             "[GameModManager]\nfolder=ModB\npriority=2\nsource_type=manual\n"
             "source_id=\n");
  // Orphan: sidecar with no matching mod folder.
  write_file(legacy_dir / "Orphan.ini",
             "[GameModManager]\nfolder=Orphan\npriority=1\n");
  // ModC: raw MO2 meta, no sidecar - enriched in place.
  write_file(mods_dir / "ModC" / "meta.ini",
             "[General]\ngameName=Skyrim\nrepository=Nexus\nmodid=123\n"
             "version=1.0\n");

  engine::GameKnowledge knowledge;

  ui::ModScanRequest req;
  req.knowledge = knowledge;
  req.game_id = "testgame";
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

  // ModA merged: manager keys from the sidecar, game keys from the folder.
  {
    const auto merged =
        engine::ModMeta::load_file(mods_dir / "ModA" / "meta.ini");
    check(merged.priority() == 5, "merged priority comes from the sidecar");
    check(merged.source_type() == "nexus", "merged source_type from sidecar");
    check(merged.source_id() == "42", "merged source_id from sidecar");
    check(merged.get("General", "category") == "7",
          "merged category from sidecar");
    check(merged.get("General", "version") == "9.9",
          "merged version stays folder-owned");
    check(!fs::exists(legacy_dir / "ModA.ini"), "sidecar removed after merge");
  }

  // ModB moved into the folder.
  {
    const auto moved =
        engine::ModMeta::load_file(mods_dir / "ModB" / "meta.ini");
    check(moved.priority() == 2, "moved sidecar keeps its priority");
    check(!fs::exists(legacy_dir / "ModB.ini"), "sidecar removed after move");
  }

  // Orphan swept to meta.bak/, not deleted.
  check(!fs::exists(legacy_dir / "Orphan.ini"), "orphan leaves meta/");
  check(fs::exists(base / "meta.bak" / "Orphan.ini"),
        "orphan sidecar preserved in meta.bak/");

  // ModC enriched in place: GameModManager section stamped, game keys kept.
  {
    const auto enriched =
        engine::ModMeta::load_file(mods_dir / "ModC" / "meta.ini");
    check(enriched.source_type() == "nexus",
          "raw MO2 meta enriched with Nexus source");
    check(enriched.source_id() == "123", "enriched source_id is the modid");
    check(enriched.get("GameModManager", "folder") == "ModC",
          "enriched folder key stamped");
  }

  fs::remove_all(base, ec);
}

// Workspace-pmrh M1: meta.bak/ is a rescue copy, not per-scan scratch.
// A second scan must not wipe it, and the orphan sweep must never
// overwrite an existing rescue copy with a same-named sidecar.
TEST_CASE("mod scan worker preserves meta.bak across scans", "[ui]") {
  int test_argc = 1;
  char test_argv0[] = "test";
  char *test_argv[] = {test_argv0, nullptr};
  QCoreApplication app(test_argc, test_argv);
  (void)app;

  const fs::path base = fs::current_path() / ("gmm_test_mod_scan_bak_" +
                                              std::to_string(getpid()));
  const fs::path mods_dir = base / "mods";
  const fs::path legacy_dir = base / "meta";
  std::error_code ec;
  fs::create_directories(mods_dir / "ModA", ec);
  fs::create_directories(legacy_dir, ec);

  // Fresh orphan sidecar + a pre-existing rescue copy of the same name.
  write_file(legacy_dir / "Orphan.ini",
             "[GameModManager]\nfolder=Orphan\npriority=1\n");
  write_file(base / "meta.bak" / "Orphan.ini",
             "[GameModManager]\nfolder=Orphan\npriority=99\n");

  engine::GameKnowledge knowledge;
  ui::ModScanThread thread(&app);
  ui::ModScanWorker *worker = thread.worker();
  std::vector<ui::ModScanResult> results;
  QObject::connect(worker, &ui::ModScanWorker::finished, &app,
                   [&](ui::ModScanResult result, quint64) {
                     results.push_back(std::move(result));
                   });
  auto run_scan = [&](quint64 generation) {
    ui::ModScanRequest req;
    req.knowledge = knowledge;
    req.game_id = "testgame";
    req.instance_root = base;
    req.mods_dir = mods_dir;
    thread.start(std::move(req), generation);
    QElapsedTimer timer;
    timer.start();
    while (results.size() < generation) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
      QThread::msleep(2);
      if (timer.elapsed() > 10000) {
        FAIL("scan never landed");
      }
    }
  };

  run_scan(1);

  // The rescue copy wins: not overwritten by the same-named sidecar, and
  // the sidecar is left in place for a later retry.
  check(engine::ModMeta::load_file(base / "meta.bak" / "Orphan.ini")
            .priority() == 99,
        "orphan sweep never overwrites an existing rescue copy");
  check(fs::exists(legacy_dir / "Orphan.ini"),
        "skipped orphan sidecar stays in meta/ for the next scan");

  // A second scan must not wipe the rescue window.
  run_scan(2);
  check(engine::ModMeta::load_file(base / "meta.bak" / "Orphan.ini")
            .priority() == 99,
        "meta.bak/ survives a second scan");

  fs::remove_all(base, ec);
}

// Workspace-7tjz: Isaac-style game-dir-only mods must not lose their manager
// state to the migration orphan sweep. A sidecar whose folder exists ONLY in
// the external game-mods dir migrates into that folder (never to meta.bak/,
// never as an instance stub). Rescue copies in meta.bak/ whose folder exists
// again are restored next to the mod; copies shadowing a newer in-folder
// meta.ini, or with no folder anywhere, stay in meta.bak/.
TEST_CASE("mod scan worker keeps game-dir sidecars out of meta.bak", "[ui]") {
  int test_argc = 1;
  char test_argv0[] = "test";
  char *test_argv[] = {test_argv0, nullptr};
  QCoreApplication app(test_argc, test_argv);
  (void)app;

  const fs::path base = fs::current_path() / ("gmm_test_mod_scan_gamedir_" +
                                              std::to_string(getpid()));
  const fs::path mods_dir = base / "mods";
  const fs::path game_mods = base / "game" / "mods";
  const fs::path legacy_dir = base / "meta";
  std::error_code ec;
  fs::create_directories(mods_dir, ec);
  fs::create_directories(legacy_dir, ec);
  fs::create_directories(game_mods / "WorkshopMod", ec);

  // Sidecar for a mod that lives only in the game-mods dir (the Isaac
  // Steam Workshop shape): full manager state that must survive.
  write_file(legacy_dir / "WorkshopMod.ini",
             "[GameModManager]\nfolder=WorkshopMod\npriority=3\n"
             "source_type=steam\nsource_id=12345\n\n[General]\ncategory=7\n");

  // Buried rescue copy whose folder exists again (game-dir only).
  write_file(base / "meta.bak" / "Buried.ini",
             "[GameModManager]\nfolder=Buried\npriority=4\n"
             "source_type=steam\nsource_id=999\n");
  fs::create_directories(game_mods / "Buried", ec);

  // Rescue copy shadowing a newer in-folder meta.ini: must stay buried.
  write_file(base / "meta.bak" / "Shadowed.ini",
             "[GameModManager]\nfolder=Shadowed\npriority=1\n");
  fs::create_directories(game_mods / "Shadowed", ec);
  write_file(game_mods / "Shadowed" / "meta.ini",
             "[General]\nversion=2.0\n\n[GameModManager]\nfolder=Shadowed\n"
             "priority=8\n");

  // Rescue copy with no folder anywhere: stays in meta.bak/.
  write_file(base / "meta.bak" / "Ghost.ini",
             "[GameModManager]\nfolder=Ghost\npriority=1\n");

  engine::GameKnowledge knowledge;
  ui::ModScanThread thread(&app);
  ui::ModScanWorker *worker = thread.worker();
  std::vector<ui::ModScanResult> results;
  QObject::connect(worker, &ui::ModScanWorker::finished, &app,
                   [&](ui::ModScanResult result, quint64) {
                     results.push_back(std::move(result));
                   });
  {
    ui::ModScanRequest req;
    req.knowledge = knowledge;
    req.game_id = "testgame";
    req.instance_root = base;
    req.mods_dir = mods_dir;
    req.game_mods_dir = game_mods;
    thread.start(std::move(req), /*generation=*/1);
  }
  QElapsedTimer timer;
  timer.start();
  while (results.empty()) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QThread::msleep(2);
    if (timer.elapsed() > 10000) {
      FAIL("scan never landed");
    }
  }

  // The game-dir sidecar migrated next to its mod - not to meta.bak/.
  check(!fs::exists(legacy_dir / "WorkshopMod.ini"),
        "game-dir sidecar leaves meta/");
  const auto moved =
      engine::ModMeta::load_file(game_mods / "WorkshopMod" / "meta.ini");
  check(moved.source_type() == "steam",
        "migrated game-dir meta keeps source_type");
  check(moved.source_id() == "12345", "migrated game-dir meta keeps source_id");
  check(moved.get("General", "category") == "7",
        "migrated game-dir meta keeps category");
  check(!fs::exists(base / "meta.bak" / "WorkshopMod.ini"),
        "game-dir mod is not swept to meta.bak/");
  check(!fs::exists(mods_dir / "WorkshopMod"),
        "no instance stub is created for a game-dir mod");

  // The buried rescue copy is restored next to its mod.
  check(!fs::exists(base / "meta.bak" / "Buried.ini"),
        "restored rescue copy leaves meta.bak/");
  const auto revived =
      engine::ModMeta::load_file(game_mods / "Buried" / "meta.ini");
  check(revived.source_id() == "999",
        "restored game-dir meta keeps source_id");

  // Newer in-folder state wins; the rescue copy stays for manual restore.
  check(fs::exists(base / "meta.bak" / "Shadowed.ini"),
        "rescue copy shadowing in-folder meta stays in meta.bak/");
  check(engine::ModMeta::load_file(game_mods / "Shadowed" / "meta.ini")
            .priority() == 8,
        "in-folder meta is never overwritten by a rescue copy");

  // No folder anywhere: stays buried.
  check(fs::exists(base / "meta.bak" / "Ghost.ini"),
        "rescue copy with no mod folder stays in meta.bak/");

  fs::remove_all(base, ec);
}