// v3 P1 N-records + determinism (spec section 6 / 2.1.2).
//
// (a) two fixture .so with colliding game_ids load in filename order -
//     the sorted directory walk decides who keeps game support;
// (b) one fixture .so calling register_game twice appears TWICE in
//     game_plugins() (one row per record, spec 6.2);
// (c) the existing single-game fixture (Skyrim) keeps the game_id mirror
//     intact: game_id == games[0].id and the legacy identity fields match.
//
// Uses the check() PASS/FAIL pattern (Release builds compile out assert()).

#include "engine/plugin_host/plugin_loader.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#ifndef GMM_SKYRIM_PLUGIN_PATH
#define GMM_SKYRIM_PLUGIN_PATH "SkyrimSpecialEdition.so"
#endif
#ifndef GMM_MULTI_GAME_PLUGIN_PATH
#define GMM_MULTI_GAME_PLUGIN_PATH "multi_game_plugin.so"
#endif
#ifndef GMM_COLLIDE_PLUGIN_A_PATH
#define GMM_COLLIDE_PLUGIN_A_PATH "collide_plugin_a.so"
#endif
#ifndef GMM_COLLIDE_PLUGIN_B_PATH
#define GMM_COLLIDE_PLUGIN_B_PATH "collide_plugin_b.so"
#endif

namespace fs = std::filesystem;

using engine::PluginLoader;

namespace {

void check(bool cond, const std::string &msg) {
  INFO(msg);
  REQUIRE(cond);
}

}  // namespace

TEST_CASE("P1: register_game twice yields two game_plugins() rows",
          "[plugin_n_records]") {
  PluginLoader loader;
  check(loader.load_plugin(GMM_MULTI_GAME_PLUGIN_PATH), "multi-game fixture loads");
  check(loader.plugins().size() == 1, "one plugin loaded");

  const auto rows = loader.game_plugins();
  check(rows.size() == 2,
        "two rows for two records: got " + std::to_string(rows.size()));
  check(rows[0].game_id == "MultiGameAlpha", "row 0 is the first registered game");
  check(rows[0].game_display_name == "Multi Game Alpha", "row 0 display name");
  check(rows[0].steam_appid == 480, "row 0 appid");
  check(rows[1].game_id == "MultiGameBeta", "row 1 is the second registered game");
  check(rows[1].game_display_name == "Multi Game Beta", "row 1 display name");
  check(rows[1].steam_appid == 481, "row 1 appid");
}

TEST_CASE("P1: single-game fixture keeps the game_id mirror intact",
          "[plugin_n_records]") {
  PluginLoader loader;
  check(loader.load_plugin(GMM_SKYRIM_PLUGIN_PATH), "skyrim fixture loads");
  check(loader.plugins().size() == 1, "one plugin loaded");

  const auto &p = loader.plugins()[0];
  check(p.games.size() == 1, "one record for a single register_game call");
  check(p.game_id == p.games[0].game_id, "game_id mirrors games[0].game_id");
  check(p.game_display_name == p.games[0].game_display_name,
        "display name mirrors games[0]");
  check(p.steam_appid == p.games[0].steam_appid, "appid mirrors games[0]");
  check(p.game_support == p.games[0].game_support, "support mirrors games[0]");
  check(p.game_support, "single-game fixture has game support");

  const auto rows = loader.game_plugins();
  check(rows.size() == 1, "one row for one record");
  check(rows[0].game_id == p.game_id, "row identity is the mirror identity");
}

TEST_CASE("P1: colliding game_ids resolve in filename order", "[plugin_n_records]") {
  // Scratch dir holding both colliding fixtures; load_directory must visit
  // them in sorted filename order, so collide_plugin_a.so (first registrant)
  // keeps game support and collide_plugin_b.so is refused it.
  const fs::path dir = fs::temp_directory_path() / "gmm_p1_collide_test";
  fs::create_directories(dir);
  fs::copy_file(GMM_COLLIDE_PLUGIN_A_PATH, dir / "collide_plugin_a.so",
                fs::copy_options::overwrite_existing);
  fs::copy_file(GMM_COLLIDE_PLUGIN_B_PATH, dir / "collide_plugin_b.so",
                fs::copy_options::overwrite_existing);

  {
    PluginLoader loader;
    check(loader.load_directory(dir.string()), "directory loads");
    check(loader.plugins().size() == 2, "both fixtures loaded");
    check(loader.plugins()[0].path.find("collide_plugin_a") != std::string::npos,
          "first loaded plugin is the alphabetically-first file");
    check(loader.plugins()[0].game_support, "first registrant keeps game support");
    check(!loader.plugins()[1].game_support, "second registrant is refused support");

    const auto rows = loader.game_plugins();
    check(rows.size() == 1, "one instance-backing row for the colliding id");
    check(rows[0].game_display_name == "Collide Alpha",
          "the winner is the filename-ordered first");
  }
  fs::remove_all(dir);  // the test's own scratch dir
}
