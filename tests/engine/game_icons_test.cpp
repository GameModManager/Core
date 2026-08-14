// Engine test for game-icon cache plumbing.
//
// Covers: icon_url_for reading the plugin-declared "game_icon_url" key, cache
// path resolution under <data root>/cache/icons, the cached-file short-circuit
// (no network), the zero-byte leftover cleanup, and a clean failure for an
// unreachable URL (no partial file left behind).
#include "engine/core/instance/game_icons.h"
#include "engine/core/instance/instance_utils.h"
#include "engine/game/registry/game_knowledge.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;

namespace {
void require(bool cond, const char* msg) {
    INFO(msg);
    REQUIRE(cond);
}
}

TEST_CASE("game icons", "[engine]") {
    using engine::GameKnowledge;

    // Isolate from the real cache dir: point the instances override at a temp
    // root so icon_cache_dir() derives from it.
    const fs::path root = "/tmp/gmm_game_icons/instances";
    fs::remove_all(root.parent_path());
    engine::set_instances_dir_override(root);

    // --- icon_url_for reads the declared key, empty when unset. ---
    GameKnowledge knowledge;
    require(engine::icon_url_for(knowledge, "skyrimspecialedition").empty(),
            "no declared icon key -> empty url");
    knowledge.set("skyrimspecialedition", engine::kIconUrlKey,
                  "https://cdn2.steamgriddb.com/icon/hash/32/64x64.png");
    require(engine::icon_url_for(knowledge, "skyrimspecialedition") ==
                "https://cdn2.steamgriddb.com/icon/hash/32/64x64.png",
            "declared icon url is read back");

    // --- Cache path shape: <data root>/cache/icons/<game_id>.png. ---
    require(engine::icon_cache_dir() == root.parent_path() / "cache" / "icons",
            "icon cache dir lives under the data root");
    require(engine::cached_icon_path("skyrimspecialedition") ==
                root.parent_path() / "cache" / "icons" / "skyrimspecialedition.png",
            "cached icon path is keyed by game_id");

    // --- A cached non-empty file short-circuits the download. ---
    auto cached = engine::cached_icon_path("cachedgame");
    fs::create_directories(cached.parent_path());
    {
        std::ofstream f(cached);
        f << "PNG";
    }
    std::string err;
    require(engine::ensure_icon_cached("cachedgame", "not-a-url", err),
            "existing cached file skips the download");

    // --- A zero-byte leftover is cleared and a download is attempted. ---
    auto leftover = engine::cached_icon_path("leftovergame");
    fs::create_directories(leftover.parent_path());
    std::ofstream(leftover).close();
    err.clear();
    require(!engine::ensure_icon_cached("leftovergame", "not-a-url", err),
            "zero-byte leftover is not trusted as cached");
    require(!err.empty(), "failure reports an error");
    require(!fs::exists(leftover),
            "zero-byte leftover is removed before the retry");

    // --- An unreachable URL fails cleanly and leaves no partial file. ---
    err.clear();
    require(!engine::ensure_icon_cached("freshgame",
                "http://127.0.0.1:1/icon.png", err),
            "unreachable url fails");
    require(!err.empty(), "unreachable url sets an error");
    require(!fs::exists(engine::cached_icon_path("freshgame")),
            "no partial file left behind on failure");

    // Restore defaults and clean up.
    engine::set_instances_dir_override({});
    fs::remove_all(root.parent_path());
}
