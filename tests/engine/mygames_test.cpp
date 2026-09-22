// Per-platform My Games path resolution (Workspace-skgg) and Steam userdata
// saves (Workspace-1e54, Isaac): knowledge accessors resolve through a stub
// platform so no Steam install or prefix is needed. Uses temp dirs only.
#include "engine/game/registry/game_knowledge.h"
#include "platform/platform.h"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
void require(bool cond, const std::string& msg) {
  INFO(msg);
  REQUIRE(cond);
}

class StubPlatform : public engine::Platform {
public:
  std::string os = "linux";
  fs::path native_docs;
  fs::path prefix_docs;
  fs::path userdata;

  [[nodiscard]] std::string platform_name() const override { return os; }
  [[nodiscard]] fs::path data_dir() const override { return {}; }
  [[nodiscard]] fs::path config_dir() const override { return {}; }
  [[nodiscard]] fs::path cache_dir() const override { return {}; }
  [[nodiscard]] fs::path find_steam_root() const override { return {}; }
  [[nodiscard]] fs::path native_documents_dir() const override { return native_docs; }
  [[nodiscard]] fs::path game_documents_dir(uint32_t /*steam_appid*/) const override {
    return prefix_docs;
  }
  [[nodiscard]] fs::path steam_userdata_dir() const override { return userdata; }
  [[nodiscard]] bool
  launch_executable(const fs::path& /*executable*/,
                    const std::vector<std::string>& /*args*/ = {}) const override {
    return false;
  }
  [[nodiscard]] fs::path home_dir() const override { return {}; }
  [[nodiscard]] fs::path temp_dir() const override { return fs::temp_directory_path(); }
};

static fs::path make_temp(const std::string& tag) {
  static int counter = 0;
  auto dir           = fs::temp_directory_path() /
                       ("gmm_mygames_" + tag + "_" + std::to_string(counter++));
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}
}  // namespace

TEST_CASE("mygames leaf prefers per-os hook", "[engine]") {
  engine::GameKnowledge knowledge;
  knowledge.set("game", "mygames_folder", "Generic Leaf");
  knowledge.set("game", "mygames_folder_linux", "Linux Leaf");

  require(engine::mygames_leaf_for(knowledge, "game", "linux") == "Linux Leaf",
          "leaf: linux override wins");
  require(engine::mygames_leaf_for(knowledge, "game", "macos") == "Generic Leaf",
          "leaf: macos falls back to generic");
  require(engine::mygames_leaf_for(knowledge, "game", "") == "Generic Leaf",
          "leaf: empty os tag falls back to generic");
  require(engine::mygames_leaf_for(knowledge, "unknown", "linux").empty(),
          "leaf: unknown game is empty, no display-name fallback");
}

TEST_CASE("mygames parent defaults to My Games", "[engine]") {
  engine::GameKnowledge knowledge;
  require(engine::mygames_parent_for(knowledge, "game") == "My Games",
          "parent: default");
  knowledge.set("game", "mygames_parent", "Custom Parent");
  require(engine::mygames_parent_for(knowledge, "game") == "Custom Parent",
          "parent: override");
}

TEST_CASE("resolve_mygames_dir native game uses host documents", "[engine]") {
  engine::GameKnowledge knowledge;
  knowledge.set("game", "mygames_folder", "Generic Leaf");
  knowledge.set("game", "mygames_folder_linux", "Linux Leaf");
  knowledge.set("game", "steam_appid", "489830");

  StubPlatform platform;
  const auto native    = make_temp("native");
  const auto prefix    = make_temp("prefix");
  platform.native_docs = native;
  platform.prefix_docs = prefix;

  const auto dir = engine::resolve_mygames_dir("game", knowledge, &platform, false);
  require(dir == native / "Linux Leaf", "native: per-os leaf under host docs");

  // Null platform and missing leaf resolve to empty, never a guess.
  require(engine::resolve_mygames_dir("game", knowledge, nullptr, false).empty(),
          "native: null platform is empty");
  engine::GameKnowledge bare;
  require(engine::resolve_mygames_dir("game", bare, &platform, false).empty(),
          "native: no leaf is empty, no display-name fallback");
}

TEST_CASE("resolve_mygames_dir proton game uses prefix documents", "[engine]") {
  engine::GameKnowledge knowledge;
  knowledge.set("game", "mygames_folder", "Skyrim Special Edition");
  knowledge.set("game", "mygames_folder_linux", "Linux Leaf");
  knowledge.set("game", "steam_appid", "489830");

  StubPlatform platform;
  const auto native    = make_temp("native2");
  const auto prefix    = make_temp("prefix2");
  platform.native_docs = native;
  platform.prefix_docs = prefix;

  const auto dir = engine::resolve_mygames_dir("game", knowledge, &platform, true);
  require(dir == prefix / "My Games" / "Skyrim Special Edition",
          "proton: generic leaf under prefix docs, per-os ignored");

  knowledge.set("game", "mygames_parent", "Custom Parent");
  require(engine::resolve_mygames_dir("game", knowledge, &platform, true) ==
              prefix / "Custom Parent" / "Skyrim Special Edition",
          "proton: custom parent honored");

  engine::GameKnowledge no_leaf;
  no_leaf.set("game", "steam_appid", "489830");
  require(engine::resolve_mygames_dir("game", no_leaf, &platform, true).empty(),
          "proton: empty leaf is empty, no display-name fallback");
}

TEST_CASE("resolve_mygames_dir windows host is always native", "[engine]") {
  engine::GameKnowledge knowledge;
  knowledge.set("game", "mygames_folder", "Skyrim Special Edition");
  knowledge.set("game", "steam_appid", "489830");

  StubPlatform platform;
  platform.os          = "windows";
  const auto native    = make_temp("win");
  platform.native_docs = native;

  // Even a .exe on native Windows resolves under the host Documents dir.
  require(engine::resolve_mygames_dir("game", knowledge, &platform, true) ==
              native / "Skyrim Special Edition",
          "windows: exe resolves natively");
}

TEST_CASE("steam userdata saves skip account zero", "[engine]") {
  engine::GameKnowledge knowledge;
  knowledge.set("isaac", "steam_appid", "250900");
  knowledge.set("isaac", "steam_userdata_saves", "remote");

  StubPlatform platform;
  const auto userdata = make_temp("userdata");
  fs::create_directories(userdata / "0" / "250900" / "remote");
  fs::create_directories(userdata / "999" / "250900" / "remote");
  fs::create_directories(userdata / "123" / "250900" / "remote");
  platform.userdata = userdata;

  const auto dir =
      engine::resolve_steam_userdata_saves_dir("isaac", knowledge, &platform);
  require(dir == userdata / "123" / "250900" / "remote",
          "userdata: first non-zero user wins, 0 skipped");

  engine::GameKnowledge plain;
  plain.set("skyrim", "steam_appid", "489830");
  require(engine::resolve_steam_userdata_saves_dir("skyrim", plain, &platform).empty(),
          "userdata: no hook declares nothing");
  require(engine::resolve_steam_userdata_saves_dir("isaac", knowledge, nullptr).empty(),
          "userdata: null platform is empty");

  fs::remove_all(userdata);
}

TEST_CASE("save extensions default to ess", "[engine]") {
  engine::GameKnowledge knowledge;
  require(engine::save_extensions_for(knowledge, "skyrim") ==
              std::vector<std::string>{"ess"},
          "extensions: undeclared game defaults to ess");
  require(engine::save_extensions_for(knowledge, "unknown") ==
              std::vector<std::string>{"ess"},
          "extensions: unknown game defaults to ess");
}

TEST_CASE("save extensions parse the hook", "[engine]") {
  engine::GameKnowledge knowledge;
  knowledge.set("isaac", "save_extensions", "dat");
  require(engine::save_extensions_for(knowledge, "isaac") ==
              std::vector<std::string>{"dat"},
          "extensions: single hook value");

  knowledge.set("multi", "save_extensions", "ess, ess.bak");
  require(engine::save_extensions_for(knowledge, "multi") ==
              (std::vector<std::string>{"ess", "ess.bak"}),
          "extensions: comma list with whitespace");

  knowledge.set("messy", "save_extensions", " dat ,, bak ");
  require(engine::save_extensions_for(knowledge, "messy") ==
              (std::vector<std::string>{"dat", "bak"}),
          "extensions: empties dropped, whitespace trimmed");

  knowledge.set("empty", "save_extensions", " , ");
  require(engine::save_extensions_for(knowledge, "empty") ==
              std::vector<std::string>{"ess"},
          "extensions: blank hook falls back to ess");
}
