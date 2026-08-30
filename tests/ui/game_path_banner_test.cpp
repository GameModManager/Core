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
// Hermetic: offscreen platform, throwaway /tmp instance root, an empty
// GameKnowledge seeded with just mods_subpath. No network, no plugins.
#include "engine/core/instance/instance.h"
#include "engine/game/registry/game_knowledge.h"
#include "ui/controllers/mod_list_controller.h"
#include "ui/main_window/main_window.h"
#include "ui/widgets/game_path_banner.h"
#include "ui/widgets/mod_list_model.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>

namespace {

const auto kInstancesRoot = "/tmp/gmm_tnj_teardown/instances";

// Creates <root>/TestGame with dirs + instance.toml (game_id set, game_dir
// deliberately empty) — the on-disk shape of a game-less instance.
std::filesystem::path make_instance() {
    std::filesystem::remove_all("/tmp/gmm_tnj_teardown");
    auto inst = engine::Instance::installed("TestGame", kInstancesRoot);
    inst.info().game_id = "testgame";
    REQUIRE(inst.create_directories());
    REQUIRE(inst.write_toml());
    return inst.info().root;
}

} // namespace

TEST_CASE("set_game_info with empty game dir keeps the UI alive", "[ui]") {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    int test_argc = 1;
    char test_argv0[] = "test";
    char* test_argv[] = {test_argv0, nullptr};
    QApplication app(test_argc, test_argv);
    QCoreApplication::setOrganizationName("GameModManager");
    QCoreApplication::setApplicationName("GameModManager");

    const auto root = make_instance();

    ui::MainWindow w;
    engine::GameKnowledge knowledge;
    knowledge.set("testgame", "mods_subpath", "Mods");
    w.set_game_knowledge(&knowledge);
    w.show();

    // Must not crash and must leave a usable window behind.
    w.set_game_info("testgame", "Test Game", "", {}, root);

    auto* banner = w.findChild<ui::GamePathBanner*>();
    REQUIRE(banner != nullptr);
    CHECK(banner->isVisible());

    // The Default profile was bootstrapped by refresh_profiles().
    CHECK(std::filesystem::is_directory(root / "profiles" / "Default"));

    // A later load WITH a game dir hides the banner again.
    w.set_game_info("testgame", "Test Game", "", "/tmp/gmm_tnj_teardown/game",
                    root);
    CHECK_FALSE(banner->isVisible());
}

TEST_CASE("instance-owned mod ops work without a game dir", "[ui]") {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    int test_argc = 1;
    char test_argv0[] = "test";
    char* test_argv[] = {test_argv0, nullptr};
    QApplication app(test_argc, test_argv);
    QCoreApplication::setOrganizationName("GameModManager");
    QCoreApplication::setApplicationName("GameModManager");

    const auto root = make_instance();
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

    auto* ctrl = w.findChild<ui::ModListController*>();
    REQUIRE(ctrl != nullptr);
    auto* model = w.findChild<ui::ModList*>();
    REQUIRE(model != nullptr);

    // Separator creation used to be blocked by the empty-game_dir guard.
    const auto sep_id = ctrl->create_separator_named("Cool", "");
    CHECK(sep_id == QString("Cool_separator"));
    CHECK(std::filesystem::is_directory(mods_dir / "Cool_separator"));
    const auto sep_row = std::find_if(model->mods().cbegin(),
                                      model->mods().cend(),
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

    // Priority sync persists to the meta sidecar without a game dir. The
    // write only happens when a row's priority actually changes away from
    // the default 0, so move the separator below the Overwrite row.
    model->move_mod(QString("Renamed_separator"), 1);
    ctrl->sync_priorities();
    CHECK(std::filesystem::exists(root / "meta" / "Renamed_separator.ini"));
}

// Workspace-wk8: the mod list loads for a game-less instance. The scan
// runs against the instance mods dir (ModScanWorker swaps it in when
// game_dir is empty), so a mod folder seeded there shows up.
TEST_CASE("mod list loads from instance mods dir without a game dir", "[ui]") {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    int test_argc = 1;
    char test_argv0[] = "test";
    char* test_argv[] = {test_argv0, nullptr};
    QApplication app(test_argc, test_argv);
    QCoreApplication::setOrganizationName("GameModManager");
    QCoreApplication::setApplicationName("GameModManager");

    const auto root = make_instance();
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

    auto* model = w.findChild<ui::ModList*>();
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
        found = std::any_of(mods.cbegin(), mods.cend(),
                            [](const auto &m) { return m.id == "Foo_mod"; });
        if (!found && timer.elapsed() > 10000)
            FAIL("mod scan never landed for the game-less instance");
    }
}
