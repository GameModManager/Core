// A failed mod-enable sentinel write has to say WHY.
//
// The sentinel is the only on-disk record that a mod is off, so a refused
// write leaves the row describing a state the game will never see. The engine
// used to return a bare bool, and the only trace was a log line reading
// "could not write the sentinel" - which tells the user nothing they can act
// on. These cases pin the reason to the OS message the write actually got.
//
// The game is deliberately NOT Isaac. Isaac's plugin is the one game that
// declares its own `disable_mechanism`, so pinning only that would leave the
// default path (every other game) untested. Here the knowledge store says
// nothing, so the engine default applies - which is exactly the code path the
// other games take.

#include "engine/game/detect/mod_scanner.h"
#include "engine/game/registry/game_knowledge.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace {

// A game id no plugin claims, so disable_mechanism_for() falls through to
// kDefaultDisableMechanism - the same resolution every game without a
// declared sentinel name gets.
constexpr const char *kGenericGame = "SomeGameWithNoPlugin";

// Per-case throwaway root under /tmp, removed on scope exit even when an
// assertion FAILs, so a red run leaves nothing behind.
struct CaseRoot {
  explicit CaseRoot(const char *name) : root(fs::path("/tmp") / name) {
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);
  }
  ~CaseRoot() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
  CaseRoot(const CaseRoot &)            = delete;
  CaseRoot &operator=(const CaseRoot &) = delete;

  fs::path root;
};

}  // namespace

// The reason must name what was attempted (the sentinel), where (the folder
// the toggle resolved to) and what the OS said. Asserting the content is the
// point: a boolean "did it produce a reason" would pass against a string the
// UI composed out of nothing.
TEST_CASE("A sentinel write that cannot land reports the OS reason, for any game",
          "[engine][mod_scanner][toggle]") {
  CaseRoot root("gmm_toggle_reason_missing");
  engine::GameKnowledge knowledge;
  const auto mod_folder = root.root / "NoSuchMod";

  // The engine default sentinel name, resolved the same way the toggle does.
  CHECK(knowledge.get(kGenericGame, "disable_mechanism",
                      engine::kDefaultDisableMechanism) ==
        std::string(engine::kDefaultDisableMechanism));

  std::string reason;
  REQUIRE_FALSE(engine::ModScanner::disable_mod(knowledge, kGenericGame, mod_folder,
                                                &reason));

  // Names the sentinel, so the user knows which file the app wanted.
  CHECK(reason.find(engine::kDefaultDisableMechanism) != std::string::npos);
  // Names the folder, so the user knows which mod.
  CHECK(reason.find(mod_folder.string()) != std::string::npos);
  // And says what the OS said, rather than "could not write".
  CHECK(reason.find("could not write") != std::string::npos);
  CHECK(reason.find("No such file or directory") != std::string::npos);
}

// The other half of the same defect: re-enabling removes the sentinel, and a
// removal that is refused (a non-empty directory sitting where the sentinel
// should be) used to be indistinguishable from "there was nothing to remove".
// A directory is the deterministic way to make remove() fail with a real OS
// reason - it needs no permissions, so it holds as root too.
TEST_CASE("A sentinel removal that is refused reports why it was not already gone",
          "[engine][mod_scanner][toggle]") {
  CaseRoot root("gmm_toggle_reason_remove");
  engine::GameKnowledge knowledge;
  const auto mod_folder = root.root / "Mod";
  fs::create_directories(mod_folder / engine::kDefaultDisableMechanism);
  // A non-empty directory is ENOTEMPTY for remove(), where a file would be
  // removed cleanly and prove nothing.
  std::ofstream(mod_folder / engine::kDefaultDisableMechanism / "blocker")
      << "x";

  std::string reason;
  REQUIRE_FALSE(engine::ModScanner::enable_mod(knowledge, kGenericGame, mod_folder,
                                               &reason));
  CHECK(reason.find(engine::kDefaultDisableMechanism) != std::string::npos);
  CHECK(reason.find(mod_folder.string()) != std::string::npos);
  CHECK(reason.find("Directory not empty") != std::string::npos);
}

// NEGATIVE CONTROL. The reason is a failure report, not a status line: a write
// that succeeds, and a removal of an already-absent sentinel, must both leave
// it untouched. Without this, a regression that reports "could not write"
// unconditionally would pass every case above.
TEST_CASE("A sentinel write and removal that succeed leave the reason empty",
          "[engine][mod_scanner][toggle]") {
  CaseRoot root("gmm_toggle_reason_negative");
  engine::GameKnowledge knowledge;
  const auto mod_folder = root.root / "Mod";
  fs::create_directories(mod_folder);
  const fs::path sentinel = mod_folder / engine::kDefaultDisableMechanism;
  REQUIRE_FALSE(fs::exists(sentinel));

  std::string reason = "untouched";
  REQUIRE(engine::ModScanner::disable_mod(knowledge, kGenericGame, mod_folder,
                                          &reason));
  CHECK(reason == "untouched");
  CHECK(fs::exists(sentinel));

  // Removing it is the success case, and it must still say nothing.
  REQUIRE(engine::ModScanner::enable_mod(knowledge, kGenericGame, mod_folder,
                                         &reason));
  CHECK(reason == "untouched");
  CHECK_FALSE(fs::exists(sentinel));

  // Removing it a second time is NOT a failure - it is the state the toggle
  // asked for. The engine returns false, and the reason must stay empty so
  // the caller can tell this apart from a refused write.
  REQUIRE_FALSE(
      engine::ModScanner::enable_mod(knowledge, kGenericGame, mod_folder, &reason));
  CHECK(reason == "untouched");
}
