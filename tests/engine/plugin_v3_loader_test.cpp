#include "engine/plugin_host/plugin_loader.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#ifndef GMM_V3_LOADER_FIXTURE_PATH
#define GMM_V3_LOADER_FIXTURE_PATH "v3_loader_fixture.so"
#endif
#ifndef GMM_V3_VERSION_MISMATCH_FIXTURE_PATH
#define GMM_V3_VERSION_MISMATCH_FIXTURE_PATH "v3_version_mismatch_fixture.so"
#endif
#ifndef GMM_SKYRIM_PLUGIN_PATH
#define GMM_SKYRIM_PLUGIN_PATH "SkyrimSpecialEdition.so"
#endif

namespace fs = std::filesystem;
using engine::PluginLoader;

namespace {
void check(bool cond, const std::string &msg) {
  INFO(msg);
  REQUIRE(cond);
}
}  // namespace

TEST_CASE("P3: v3 fixture loads and registers 2 games + 2 capabilities",
          "[plugin_v3_loader]") {
  PluginLoader loader;
  check(loader.load_plugin(GMM_V3_LOADER_FIXTURE_PATH), "v3 fixture loads");
  check(loader.plugins().size() == 1, "one plugin loaded");
  const auto &p = loader.plugins()[0];
  check(p.abi_version == 3, "abi_version is 3");
  check(p.games.size() == 2, "two game records");
  check(p.games[0].game_id == "V3Alpha", "first game id");
  check(p.games[1].game_id == "V3Beta", "second game id");
  check(p.v3_cap_count == 2, "two capabilities");
  check(p.v3_caps != nullptr, "caps pointer set");

  const auto rows = loader.game_plugins();
  check(rows.size() == 2, "two game_plugins rows");
}

TEST_CASE("P3: v2 fixture still loads on the same build", "[plugin_v3_loader]") {
  PluginLoader loader;
  check(loader.load_plugin(GMM_SKYRIM_PLUGIN_PATH), "skyrim v2 fixture loads");
  check(loader.plugins()[0].abi_version == 2, "abi_version is 2");
}

TEST_CASE("P3: version mismatch is refused cleanly", "[plugin_v3_loader]") {
  PluginLoader loader;
  check(!loader.load_plugin(GMM_V3_VERSION_MISMATCH_FIXTURE_PATH),
        "version-99 fixture refused");
  check(loader.plugins().empty(), "no plugin registered on refusal");
  // A non-existent path must also fail without crashing.
  check(!loader.load_plugin("/tmp/opencode/no_such_plugin.so"), "missing file refused");
}