#include "engine/index/conflict_engine.h"

#include <filesystem>
#include <fstream>

#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;

// Workspace-j4yj regression: Isaac-style mods whose instance folder holds
// only meta.ini while the real content lives in the game's own mods dir.
// ConflictEngine::compute() must walk BOTH dirs and merge - before the
// dual-dir walk only the instance dir was walked unless it was missing
// entirely, and stubs exist, so every mod walked to an empty file list and
// the registry (Data tab + Conflicts) came back empty.
TEST_CASE("conflict_external_merge", "[engine]") {
  using namespace engine;

  fs::path base = fs::temp_directory_path() / "conflict_external_merge_core";
  fs::remove_all(base);
  fs::path inst = base / "isaac_inst";
  fs::path game = base / "isaac_game";
  fs::create_directories(inst / "stub_a");
  fs::create_directories(inst / "stub_b");
  fs::create_directories(game / "stub_a" / "resources");
  fs::create_directories(game / "stub_b" / "resources");

  auto touch = [](const fs::path &p) {
    std::ofstream(p).put('\n');
  };
  touch(inst / "stub_a" / "meta.ini");
  touch(inst / "stub_b" / "meta.ini");
  touch(game / "stub_a" / "metadata.xml");
  touch(game / "stub_b" / "metadata.xml");
  touch(game / "stub_a" / "main.lua");
  touch(game / "stub_b" / "main.lua");
  touch(game / "stub_a" / "resources" / "shared.txt");
  touch(game / "stub_b" / "resources" / "shared.txt");
  touch(game / "stub_a" / "resources" / "unique_a.txt");

  std::vector<ConflictEngine::ModInfo> mods = {{"stub_a", 1}, {"stub_b", 2}};
  ConflictEngine engine;
  fs::path cache = base / "conflict_cache.json";
  auto results   = engine.compute(inst, mods, "", "meta.ini,metadata.xml,disable.it",
                                  true, cache, game, "resources");

  INFO("one entry per mod");
  REQUIRE(results.size() == 2);
  INFO("stub_a merges stub meta.ini skip + external files");
  REQUIRE(results["stub_a"].total_files == 2);
  INFO("stub_b merges its single shared file");
  REQUIRE(results["stub_b"].total_files == 1);

  const auto &reg = engine.last_registry();
  INFO("registry non-empty, Data tab has content");
  REQUIRE(!reg.empty());
  auto shared = reg.find("resources/shared.txt");
  INFO("shared.txt owned by both mods");
  REQUIRE(shared != reg.end());
  REQUIRE(shared->second.size() == 2);
  INFO("root-level files outside scan dirs excluded");
  REQUIRE(reg.find("main.lua") == reg.end());
  INFO("reversed: lower priority number wins shared.txt");
  REQUIRE(results["stub_a"].wins == 1);
  INFO("reversed: higher priority number loses shared.txt");
  REQUIRE(results["stub_b"].losses == 1);

  fs::remove_all(base);
}
