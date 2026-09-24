// Engine test for per-instance appearance/plugin settings (Workspace-1065).
//
// Covers the instance.toml schema:
//   [appearance] theme/style/icon_pack (empty = unset, section omitted)
//   [plugins] disabled = [...] (nullopt = unset/global fallback, an
//     explicitly empty array overrides globals)
//   [plugin_options] nested tables + dotted keys (plugin1.option1 = "value")
// and that unrelated keys survive the read-modify-write roundtrip.
#include "engine/core/instance/instance.h"
#include "engine/core/instance/toml_utils.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;

namespace {

void require(bool cond, const char *msg) {
  INFO(msg);
  REQUIRE(cond);
}

std::string read_file(const fs::path &path) {
  std::ifstream in(path);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}  // namespace

TEST_CASE("instance per-instance settings schema", "[engine]") {
  using engine::Instance;

  const fs::path root = "/tmp/gmm_instance_settings/instances/Test";
  fs::remove_all(root);
  fs::create_directories(root);

  // --- Unset by default: nothing persisted. ---
  Instance inst       = Instance::from_root(root);
  inst.info().game_id = "test_game";
  require(inst.write_toml(), "write_toml succeeds");
  const std::string bare = read_file(root / "instance.toml");
  require(bare.find("appearance") == std::string::npos,
          "no [appearance] section when unset");
  require(bare.find("plugin_options") == std::string::npos,
          "no [plugin_options] section when unset");
  require(bare.find("[plugins]") == std::string::npos,
          "no [plugins] section when disabled list is nullopt");

  Instance back = Instance::from_root(root);
  require(back.read_toml(), "read_toml succeeds");
  require(back.info().appearance_theme.empty(), "theme unset by default");
  require(back.info().appearance_style.empty(), "style unset by default");
  require(back.info().appearance_icon_pack.empty(), "icon_pack unset by default");
  require(!back.info().plugins_disabled.has_value(),
          "disabled plugins nullopt by default");
  require(back.info().plugin_options.empty(), "plugin options empty by default");

  // --- Full roundtrip. ---
  inst.info().appearance_theme     = "dark";
  inst.info().appearance_style     = "Fusion";
  inst.info().appearance_icon_pack = "Fugue";
  inst.info().plugins_disabled = std::vector<std::string>{"plugin1.so", "plugin2.so"};
  inst.info().plugin_options["plugin1.so"]["option1"] = "value";
  inst.info().plugin_options["plugin1.so"]["option2"] = "1";
  require(inst.write_toml(), "write_toml with settings succeeds");

  Instance full = Instance::from_root(root);
  require(full.read_toml(), "read_toml with settings succeeds");
  require(full.info().appearance_theme == "dark", "theme roundtrips");
  require(full.info().appearance_style == "Fusion", "style roundtrips");
  require(full.info().appearance_icon_pack == "Fugue", "icon_pack roundtrips");
  require(full.info().plugins_disabled.has_value(), "disabled list is set");
  require(full.info().plugins_disabled->size() == 2, "disabled list keeps entries");
  require((*full.info().plugins_disabled)[0] == "plugin1.so",
          "disabled list order preserved");
  require(full.info().plugin_options["plugin1.so"]["option1"] == "value",
          "plugin option roundtrips");
  require(full.info().plugin_options["plugin1.so"]["option2"] == "1",
          "second plugin option roundtrips");
  require(full.info().game_id == "test_game",
          "unrelated keys survive alongside new sections");

  // --- Explicitly empty disabled list overrides (stays set, not fallback).
  // ---
  inst.info().plugins_disabled = std::vector<std::string>{};
  require(inst.write_toml(), "write_toml with empty disabled succeeds");
  Instance emptied = Instance::from_root(root);
  require(emptied.read_toml(), "read_toml with empty disabled succeeds");
  require(emptied.info().plugins_disabled.has_value(),
          "empty disabled list is still an explicit override");
  require(emptied.info().plugins_disabled->empty(),
          "empty disabled list reads back empty");

  // --- Clearing appearance removes the section. ---
  inst.info().appearance_theme     = {};
  inst.info().appearance_style     = {};
  inst.info().appearance_icon_pack = {};
  inst.info().plugin_options.clear();
  require(inst.write_toml(), "write_toml after clearing succeeds");
  const std::string cleared = read_file(root / "instance.toml");
  require(cleared.find("appearance") == std::string::npos,
          "[appearance] removed once all keys are empty");
  require(cleared.find("plugin_options") == std::string::npos,
          "[plugin_options] removed once the map is empty");

  // --- Dotted keys parse to nested tables (ticket schema example). ---
  {
    std::ofstream out(root / "instance.toml");
    out << "game_id = \"test_game\"\n"
           "[appearance]\n"
           "theme = \"dark\"\n"
           "style = \"\"\n"
           "icon_pack = \"Fugue\"\n"
           "[plugins]\n"
           "disabled = [\"plugin1\", \"plugin2\"]\n"
           "[plugin_options]\n"
           "plugin1.option1 = \"value\"\n";
    out.close();
    engine::invalidate_instance_toml_cache(root / "instance.toml");
  }
  Instance dotted = Instance::from_root(root);
  require(dotted.read_toml(), "read_toml with dotted keys succeeds");
  require(dotted.info().appearance_theme == "dark", "dotted theme reads");
  require(dotted.info().appearance_style.empty(),
          "explicit empty style reads as unset");
  require(dotted.info().appearance_icon_pack == "Fugue", "dotted icon_pack reads");
  require(dotted.info().plugins_disabled.has_value(), "dotted disabled is set");
  require(dotted.info().plugins_disabled->size() == 2, "dotted disabled keeps both");
  require(dotted.info().plugin_options["plugin1"]["option1"] == "value",
          "dotted plugin option parses to nested tables");

  fs::remove_all(root);
}
