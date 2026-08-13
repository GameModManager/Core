// Plugin-DB preload worker test (P8.5 / T6).
//
// Pins the startup-parallelization contracts the plugin-DB load must hold:
//   1. The load (PluginDatabase::refresh -> TES4 header parse -> creation-club
//      -> sort_load_order) runs on the dedicated `gmm-plugin-db` worker thread,
//      NOT the main thread — proven two ways:
//        - the finished() sender lives on a non-main QThread named gmm-plugin-db
//          (queued invokeMethod guarantees run() executes there);
//        - a 0ms single-shot armed BEFORE start() has already fired by the time
//          finished() lands (the main thread was pumping its event loop the
//          whole time the load was running on the worker).
//   2. The parsed database is correct end-to-end: game-native plugins are
//      force-loaded and first, Creation Club content (skyrim.ccc) is flagged,
//      mod-owned plugins get owner_mod, a disabled mod (disable.it sentinel)
//      contributes nothing, and the native/CC/user band order is preserved.
//   3. finished() carries the exact generation of the run that produced it, so
//      a superseding load (MainWindow's generation guard) can drop a stale one.
//
// Hermetic: QCoreApplication (no widgets), synthetic plugin files written by a
// TES4 writer, throwaway temp dir under the build dir.
#include "ui/main_window/plugin_db_load_worker.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QTimer>

#include "engine/log/logger.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>
#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;

namespace {
void check(bool cond, const char* what) {
    INFO(what);
    REQUIRE(cond);
}
}

namespace {

void append_u16(std::vector<char>& v, uint16_t x) {
    v.push_back(static_cast<char>(x & 0xFF));
    v.push_back(static_cast<char>((x >> 8) & 0xFF));
}

// Minimal valid TES4 record (same shape as plugin_database_test's writer):
// TES4 magic + MAST subrecords (2-byte sizes) + an esm flag where asked.
void write_esp(const fs::path& path, bool esm_flag,
               const std::vector<std::string>& masters) {
    std::vector<char> body;
    for (const auto& m : masters) {
        body.push_back('M');
        body.push_back('A');
        body.push_back('S');
        body.push_back('T');
        append_u16(body, static_cast<uint16_t>(m.size() + 1));
        body.insert(body.end(), m.begin(), m.end());
        body.push_back('\0');
    }
    std::ofstream out(path, std::ios::binary);
    out.write("TES4", 4);
    const uint32_t data_size = static_cast<uint32_t>(body.size());
    out.write(reinterpret_cast<const char*>(&data_size), 4);
    const uint32_t flags = esm_flag ? 1u : 0u;
    out.write(reinterpret_cast<const char*>(&flags), 4);
    const uint32_t zero = 0;
    out.write(reinterpret_cast<const char*>(&zero), 4);
    out.write(reinterpret_cast<const char*>(&zero), 4);
    out.write(reinterpret_cast<const char*>(&zero), 4);
    out.write(body.data(), static_cast<std::streamsize>(body.size()));
}

}  // namespace

// Mirrors MainWindow's stale-load drop rule (on_plugin_db_preloaded): a result
// is kept only when its generation is the latest launched AND a load is still
// pending (not yet consumed by a refresh).
static bool should_accept(quint64 generation, quint64 latest_generation,
                          bool pending) {
    return generation == latest_generation && pending;
}

TEST_CASE("plugin db load", "[ui]") {
    int test_argc = 1;
    char test_argv0[] = "test";
    char* test_argv[] = {test_argv0, nullptr};
    QCoreApplication app(test_argc, test_argv);
    (void)app;

    const fs::path base =
        fs::current_path() / ("gmm_test_plugin_db_load_" + std::to_string(getpid()));
    const fs::path game_dir = base / "game";
    const fs::path data_dir = game_dir / "Data";
    const fs::path mods_dir = base / "mods";
    const fs::path meta_dir = base / "meta";
    std::error_code ec;
    fs::create_directories(data_dir, ec);
    fs::create_directories(mods_dir, ec);
    fs::create_directories(meta_dir, ec);

    // Game-native plugins: one .esm with a master (Skyrim.esm), one .esl, plus
    // a handful of extra vanilla files so the load is a real disk read (the
    // responsiveness assertion needs the worker busy for more than a single
    // event-loop turn).
    write_esp(data_dir / "Skyrim.esm", /*esm_flag=*/true, {});
    write_esp(data_dir / "Update.esm", /*esm_flag=*/true, {"Skyrim.esm"});
    write_esp(data_dir / "ccTOSSE001.esl", /*esm_flag=*/false, {"Skyrim.esm"});
    for (int i = 0; i < 40; ++i) {
        write_esp(data_dir / ("Extra" + std::to_string(i) + ".esp"),
                  /*esm_flag=*/false, {});
    }

    // Mods: "mod_a" owns a plugin that masters Skyrim.esm + Update.esm;
    // "mod_b" owns a plugin with no masters; "mod_disabled" owns a plugin but
    // carries the disable.it sentinel (must contribute nothing).
    fs::create_directories(mods_dir / "mod_a", ec);
    fs::create_directories(mods_dir / "mod_b", ec);
    fs::create_directories(mods_dir / "mod_disabled", ec);
    write_esp(mods_dir / "mod_a" / "mod_a.esp", /*esm_flag=*/false,
              {"Skyrim.esm", "Update.esm"});
    write_esp(mods_dir / "mod_b" / "mod_b.esp", /*esm_flag=*/false, {});
    write_esp(mods_dir / "mod_disabled" / "mod_disabled.esp", /*esm_flag=*/false, {});
    {
        std::ofstream out(mods_dir / "mod_disabled" / "disable.it");
        out << "disabled\n";
    }

    // skyrim.ccc (root, then Data are both tried; root wins) lists the CC esl.
    {
        std::ofstream out(game_dir / "skyrim.ccc");
        out << "ccTOSSE001.esl\n";
    }

    const std::string game_native = "Skyrim.esm,Update.esm";

    // Result collector. Receiver context = the app object (main thread), so the
    // auto connection resolves to a queued cross-thread delivery.
    struct LoadResult {
        engine::PluginDatabase db;
        quint64 generation = 0;
        QThread* worker_thread = nullptr;
    };
    std::vector<LoadResult> results;
    ui::PluginDbLoadThread thread(&app);
    ui::PluginDbLoadWorker* worker = thread.worker();

    QObject::connect(worker, &ui::PluginDbLoadWorker::finished, &app,
                     [&](engine::PluginDatabase db, quint64 generation) {
        results.push_back({std::move(db), generation, worker->thread()});
    });

    // A 0ms single-shot armed BEFORE the first start(): if the load ran on the
    // main thread, this flag could never be true when finished() lands (the
    // main thread would be inside the load, never processing the timer).
    std::atomic<bool> timer_fired{false};
    QTimer::singleShot(0, &app, [&timer_fired]() { timer_fired.store(true); });

    // --- 1) Load the fixture and prove it ran off the main thread. ---
    ui::PluginDbLoadRequest req;
    req.game_dir = game_dir;
    req.mods_dir = mods_dir;
    req.meta_dir = meta_dir;
    req.disable_mechanism = "disable.it";
    req.game_native = game_native;

    thread.start(std::move(req), /*generation=*/1);

    // Pump the main-thread event loop until the load lands.
    QElapsedTimer timer;
    timer.start();
    while (results.empty()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(2);
        if (timer.elapsed() > 10000) {
            FAIL("load never landed");
        }
    }

    check(results.size() == 1, "exactly one result for the single load");
    check(timer_fired.load(),
          "main thread kept pumping events during the load (work ran off-thread)");
    check(results[0].generation == 1, "finished() carries the run's generation");
    check(results[0].worker_thread != nullptr &&
              results[0].worker_thread != QThread::currentThread() &&
              results[0].worker_thread->objectName() == QStringLiteral("gmm-plugin-db"),
          "worker (and therefore the load) lives on the dedicated gmm-plugin-db thread");

    // --- 2) The parsed database is correct end-to-end. ---
    const auto& db = results[0].db;
    const auto has_plugin = [&db](const std::string& name) {
        return db.find(name) != nullptr;
    };
    check(has_plugin("Skyrim.esm") && has_plugin("Update.esm") &&
              has_plugin("ccTOSSE001.esl") && has_plugin("mod_a.esp") &&
              has_plugin("mod_b.esp") && has_plugin("Extra0.esp"),
          "all live plugins discovered");
    check(!has_plugin("mod_disabled.esp"),
          "disabled mod (disable.it sentinel) contributes no plugins");

    const auto* skyrim = db.find("Skyrim.esm");
    check(skyrim && skyrim->is_game_native && skyrim->force_loaded,
          "game-native ESM is flagged native + force-loaded");
    const auto* cc = db.find("ccTOSSE001.esl");
    check(cc && cc->is_cc && cc->force_loaded,
          "creation-club plugin (skyrim.ccc) is flagged CC + force-loaded");
    const auto* mod_a = db.find("mod_a.esp");
    check(mod_a && mod_a->owner_mod == "mod_a",
          "mod-owned plugin gets its owning mod folder");

    // Native / CC / user band order never goes backward (sort_load_order).
    const auto& plugins = db.plugins();
    int max_band = 0;
    bool band_ok = true;
    for (const auto& p : plugins) {
        const int band = p.is_game_native ? 0 : (p.is_cc ? 1 : 2);
        if (band < max_band) band_ok = false;
        max_band = std::max(max_band, band);
    }
    check(band_ok, "sort_load_order keeps the native / CC / user band order");
    check(mod_a != nullptr && mod_a->masters.size() == 2 &&
              mod_a->masters[0] == "Skyrim.esm",
          "TES4 MAST records parsed from mod plugins");

    // --- 3) Generation + serialization contract: two back-to-back loads. ---
    // The worker serializes queued runs FIFO; each result carries its own
    // generation, so the consumer can drop the stale (superseded) one.
    {
        ui::PluginDbLoadRequest req2;
        req2.game_dir = game_dir;
        req2.mods_dir = mods_dir;
        req2.meta_dir = meta_dir;
        req2.disable_mechanism = "disable.it";
        req2.game_native = game_native;

        thread.start(req2, /*generation=*/2);
        QElapsedTimer t2;
        t2.start();
        while (results.size() < 2) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            QThread::msleep(2);
            if (t2.elapsed() > 10000) {
                FAIL("second load never landed");
            }
        }
        check(results[1].generation == 2,
              "second load lands with its own generation (FIFO serialization)");

        // Consumer drop rule (mirrors on_plugin_db_preloaded): the stale
        // generation-1 result is dropped, the latest accepted.
        check(should_accept(results[1].generation, /*latest=*/2, /*pending=*/true) &&
                  !should_accept(results[0].generation, /*latest=*/2, /*pending=*/true),
              "stale-generation result is dropped, newest kept (generation guard)");
        check(!should_accept(2, 2, /*pending=*/false),
              "result after a synchronous fallback refresh is dropped (pending=false)");
    }

    fs::remove_all(base, ec);
}
