// Workspace-421: first-launch executable seeding must also find macOS app
// bundles living OUTSIDE game_dir (Steam can place Isaac's .app in
// ~/Applications or /Applications). seed_executable_candidates() keeps the
// game_dir hits as relative names (Workspace-6su behavior), appends
// extra-root hits as ABSOLUTE paths (every consumer resolves
// `game_dir / path`, which an absolute path bypasses), and dedupes by
// basename when both copies exist. Free function (launch_controller.h) so it
// tests without a full MainWindow; extra_roots is a parameter, so the merge
// logic is testable on any platform.
#include "ui/controllers/launch_controller.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {
void touch(const std::filesystem::path &p) {
  std::ofstream f(p);
  f << "x";
}
}  // namespace

TEST_CASE("seed_executable_candidates", "[ui]") {
  auto game = std::filesystem::temp_directory_path() / "gmm421_game";
  auto apps = std::filesystem::temp_directory_path() / "gmm421_apps";
  std::filesystem::remove_all(game);
  std::filesystem::remove_all(apps);
  std::filesystem::create_directories(game);
  std::filesystem::create_directories(apps / "The Binding of Isaac Rebirth.app");
  touch(game / "isaac-ng.exe");

  const std::string declared = "isaac-ng.exe,The Binding of Isaac Rebirth.app";

  // game_dir hit stays a relative name; outside-root hit becomes absolute.
  auto out = ui::seed_executable_candidates(game, declared, {apps});
  REQUIRE(out.size() == 2);
  CHECK(out[0] == "isaac-ng.exe");
  CHECK(out[1] == (apps / "The Binding of Isaac Rebirth.app").string());

  // Basename dedupe: when the bundle exists in BOTH places, the game_dir
  // copy wins and no duplicate entry is added.
  std::filesystem::create_directories(game / "The Binding of Isaac Rebirth.app");
  out = ui::seed_executable_candidates(game, declared, {apps});
  REQUIRE(out.size() == 2);
  CHECK(out[0] == "isaac-ng.exe");
  CHECK(out[1] == "The Binding of Isaac Rebirth.app");

  // No fallback roots -> plain Workspace-6su behavior.
  out = ui::seed_executable_candidates(game, declared, {});
  REQUIRE(out.size() == 2);
  CHECK(out[1] == "The Binding of Isaac Rebirth.app");

  // Settings > Workarounds > "Executable blacklist": a name on the list is
  // never offered as a launch target, at any root. Matching is on the
  // basename and case-insensitive, and the list is ';'-separated.
  touch(game / "Steam.exe");
  touch(apps / "Discord.exe");  // a regular file: a directory would not count
  const std::string with_blacklisted =
      "isaac-ng.exe,Steam.exe,Discord.exe,The Binding of Isaac Rebirth.app";
  SECTION("blacklist drops the entry at every root") {
    const std::string blacklist = "Steam.exe; Discord.exe";
    auto filtered =
        ui::seed_executable_candidates(game, with_blacklisted, {apps}, blacklist);
    for (const auto &entry : filtered) {
      const auto base = std::filesystem::path(entry).filename().string();
      CHECK(base != "Steam.exe");
      CHECK(base != "Discord.exe");
    }
    // isaac-ng.exe and the .app bundle survive. The bundle exists in both
    // roots and the game_dir copy wins the basename dedupe, so it stays a
    // relative name.
    REQUIRE(filtered.size() == 2);
    CHECK(filtered[0] == "isaac-ng.exe");
    CHECK(filtered[1] == "The Binding of Isaac Rebirth.app");

    // Case-insensitive, so a differently-cased entry still matches.
    auto lower_blacklist =
        ui::seed_executable_candidates(game, with_blacklisted, {apps}, "sTeAm.ExE");
    for (const auto &entry : lower_blacklist)
      CHECK(std::filesystem::path(entry).filename() != "Steam.exe");

    // An empty list is the untouched install: every declared name is offered.
    // The .app dedupes to its game_dir copy, so Discord is the only extra.
    auto unfiltered = ui::seed_executable_candidates(game, with_blacklisted, {apps});
    REQUIRE(unfiltered.size() == 4);
    CHECK(unfiltered[1] == "Steam.exe");
    CHECK(unfiltered[3] == (apps / "Discord.exe").string());
  }

  // Missing names are dropped by the underlying filter (Workspace-6su).
  out = ui::seed_executable_candidates(game, "missing.exe", {apps});
  CHECK(out.empty());

  std::filesystem::remove_all(game);
  std::filesystem::remove_all(apps);
}
