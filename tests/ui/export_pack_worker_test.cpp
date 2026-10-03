// Export wizard pack-build worker test.
//
// The wizard's Tree and Review pages are views of one engine::gmmpack build.
// Building a pack walks every exported mod's folder, parses every INI those
// mods ship and sha256's every bundled byte, so it scales with the instance
// and cannot run on the GUI thread - it goes to ExportPackThread instead
// (ConflictScanThread shape).
//
// This pins the three properties that make the move safe:
//   1. The build runs on the worker thread, not on the caller's. The finished()
//      signal is connected DirectConnection so its lambda runs on whichever
//      thread emitted it, and finished() is the last statement of run() - so a
//      different thread id there means the packer itself ran over there too.
//   2. The flag the thread raises on cancel() is the flag the packer honours,
//      so closing the wizard abandons the walk instead of blocking on it.
//   3. The counters the progress bar is drawn from carry real work, so the bar
//      cannot be satisfied by an animation that would run either way.
//
// Hermetic: temp dir under temp_directory_path(), removed at exit.
#include "ui/modpack/export_pack_worker.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "engine/mod/meta/mod_meta.h"

namespace fs      = std::filesystem;
namespace gmmpack = engine::gmmpack;
using Catch::Approx;

namespace {

struct TempDir {
  fs::path root;
  TempDir() {
    root = fs::temp_directory_path() /
           ("gmm_export_pack_worker_" + std::to_string(::getpid()));
    fs::create_directories(root);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
};

void write_file(const fs::path &p, const std::string &contents) {
  fs::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary);
  out << contents;
  REQUIRE(out.good());
}

engine::InstanceSnapshot make_snapshot() {
  engine::InstanceSnapshot snap;
  snap.game_id      = "skyrimspecialedition";
  snap.display_name = "Worker Pack";
  engine::ModTrackingEntry e;
  e.list_position              = 0;
  snap.mod_entries["LocalMod"] = e;
  return snap;
}

}  // namespace

TEST_CASE("export pack worker builds off the caller thread and cancels", "[ui]") {
  int test_argc     = 1;
  char test_argv0[] = "test";
  char *test_argv[] = {test_argv0, nullptr};
  QCoreApplication app(test_argc, test_argv);
  (void)app;

  TempDir td;
  const fs::path mods_dir = td.root / "mods";
  // A manual mod: no download identity, so bundling its folder is the only
  // way it can be exported - which is exactly the case that hashes every byte.
  write_file(mods_dir / "LocalMod" / "main.esp", std::string(4096, 'e'));
  write_file(mods_dir / "LocalMod" / "scripts" / "foo.pex", "PEX DATA");
  write_file(mods_dir / "LocalMod" / "local.ini", "[General]\nversion=1.0\n");
  engine::ModMeta meta = engine::ModMeta::from_default("LocalMod", "manual", "");
  REQUIRE(meta.save(mods_dir, "LocalMod"));

  gmmpack::PackOptions options;
  options.embed_folders.insert("LocalMod");

  ui::PackBuildRequest request;
  request.snapshot = make_snapshot();
  request.mods_dir = mods_dir;
  request.options  = options;

  std::vector<ui::PackBuildResult> results;
  std::vector<std::thread::id> builder_threads;
  const std::thread::id caller_thread = std::this_thread::get_id();

  ui::ExportPackThread thread(&app);
  QObject::connect(
      thread.worker(), &ui::ExportPackWorker::finished, &app,
      [&](ui::PackBuildResult result) {
        builder_threads.push_back(std::this_thread::get_id());
        results.push_back(std::move(result));
      },
      Qt::DirectConnection);

  thread.start(std::move(request));

  QElapsedTimer timer;
  timer.start();
  while (results.empty()) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QThread::msleep(2);
    REQUIRE(timer.elapsed() < 10000);
  }

  // 1. The packer ran on the worker thread, not here.
  REQUIRE(builder_threads.size() == 1);
  REQUIRE(builder_threads.front() != caller_thread);

  const ui::PackBuildResult &result = results.front();
  REQUIRE(result.ok);
  REQUIRE_FALSE(result.cancelled);
  REQUIRE(result.error.empty());

  // ...and it really built the pack: the bundled payload is hashed and loaded,
  // and the mod's INI was parsed into ini_edits.
  REQUIRE(result.pack.mods.size() == 1);
  const auto *embedded =
      std::get_if<gmmpack::ModSourceEmbedded>(&result.pack.mods.front().source);
  REQUIRE(embedded != nullptr);
  REQUIRE(embedded->files.size() ==
          4);  // main.esp + scripts/foo.pex + local.ini + meta.ini
  REQUIRE(result.pack.payload.size() == 4);
  REQUIRE(result.pack.ini_edits.size() == 1);

  // 2. cancel() raises the flag the packer checks, so a wizard closing mid-build
  // abandons the walk instead of blocking on it. The same flag, handed straight
  // to the engine, must make build_gmmpack stop at its first file boundary.
  thread.cancel();
  REQUIRE(thread.cancelled());

  gmmpack::PackCancel flag{true};
  flag.progress = std::make_shared<gmmpack::PackProgress>();
  const gmmpack::Gmmpack abandoned =
      gmmpack::build_gmmpack(make_snapshot(), mods_dir, options, &flag);
  REQUIRE(abandoned.payload.empty());
  // A pre-cancelled build resolves no embedded source, so the mod drops out.
  REQUIRE(abandoned.mods.empty());
  // ...and it never claimed to have done any of it: a bar reading this sink
  // stops on the last stage's opening boundary instead of walking to full.
  // That is the half of the bar that rules out an animation, so it is the half
  // worth pinning.
  REQUIRE(flag.progress->item.load() == 0);
  REQUIRE(flag.progress->fraction() < 1.0);

  // 3. The bar the wizard draws is fed by these counters, so they have to carry
  // real work: the finished run reached the last stage, counted every bundled
  // file, and the overall fraction landed on 1 rather than stopping partway.
  const std::shared_ptr<gmmpack::PackProgress> progress = thread.progress();
  REQUIRE(progress != nullptr);
  REQUIRE(static_cast<gmmpack::PackStage>(progress->stage.load()) ==
          gmmpack::PackStage::Payload);
  REQUIRE(progress->total.load() == 4);  // main.esp + foo.pex + local.ini + meta.ini
  REQUIRE(progress->item.load() == 4);
  REQUIRE(progress->fraction() == Approx(1.0));
}