#include "engine/plugin_host/plugin_loader.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#ifndef GMM_TOOL_PLUGIN_PATH
#define GMM_TOOL_PLUGIN_PATH "gmm_tool_first_plugin.so"
#endif
#ifndef GMM_GAMESUPPORT_PLUGIN_PATH
#define GMM_GAMESUPPORT_PLUGIN_PATH "gmm_game_support_plugin.so"
#endif
#ifndef GMM_MODSORTER_PLUGIN_PATH
#define GMM_MODSORTER_PLUGIN_PATH "IsaacModSorter.so"
#endif
#ifndef GMM_ISAAC_PLUGIN_PATH
#define GMM_ISAAC_PLUGIN_PATH "TheBindingOfIsaacRebirth.so"
#endif

namespace fs = std::filesystem;
using engine::PluginLoader;

namespace {
void check(bool cond, const std::string &msg) {
  INFO(msg);
  REQUIRE(cond);
}
}  // namespace

TEST_CASE("game support ownership: Tool registering first does not steal game_support",
          "[game_support_ownership]") {
  PluginLoader loader;
  check(loader.load_plugin(GMM_TOOL_PLUGIN_PATH), "tool plugin loads");
  check(loader.load_plugin(GMM_GAMESUPPORT_PLUGIN_PATH), "game support plugin loads");
  check(loader.plugins().size() == 2, "two plugins loaded");

  const auto &tool = loader.plugins()[0];
  const auto &gs   = loader.plugins()[1];

  // Tool record still exists with game_support=false.
  check(!tool.games.empty(), "tool has a game record");
  check(tool.games[0].game_id == "ToolFirstGame", "tool game_id");
  check(!tool.games[0].game_support, "tool game_support is false");

  // GameSupport plugin owns game_support=true.
  check(!gs.games.empty(), "game support has a game record");
  check(gs.games[0].game_id == "ToolFirstGame", "game support game_id");
  check(gs.games[0].game_support, "game support game_support is true");

  // game_plugins() exposes only the GameSupport row.
  const auto rows = loader.game_plugins();
  check(rows.size() == 1, "one game_plugins row");
  check(rows[0].game_id == "ToolFirstGame", "row game_id");
}

TEST_CASE("game support ownership: real IsaacModSorter does not steal from "
          "TheBindingOfIsaacRebirth",
          "[game_support_ownership]") {
  PluginLoader loader;
  check(loader.load_plugin(GMM_MODSORTER_PLUGIN_PATH), "mod sorter loads");
  check(loader.load_plugin(GMM_ISAAC_PLUGIN_PATH), "isaac game support loads");
  check(loader.plugins().size() == 2, "two plugins loaded");

  // Find records for the shared game_id.
  bool sorter_support = false;
  bool isaac_support  = false;
  bool sorter_record  = false;
  for (const auto &p : loader.plugins()) {
    for (const auto &rec : p.games) {
      if (rec.game_id != "TheBindingOfIsaacRebirth")
        continue;
      if (p.path.find("IsaacModSorter") != std::string::npos) {
        sorter_record  = true;
        sorter_support = rec.game_support;
      } else {
        isaac_support = rec.game_support;
      }
    }
  }
  check(sorter_record, "mod sorter has a record for the game");
  check(!sorter_support, "mod sorter game_support is false");
  check(isaac_support, "isaac game_support is true");
}
