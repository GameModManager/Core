// Tests for engine::Install game match validation: identical games match,
// mismatches block with a clear error, and normalization (case, whitespace,
// aliases) keeps equivalent ids equal.
#include "engine/install/game_match_validator.h"

#include <catch2/catch_test_macros.hpp>

using engine::Install::normalize_game_id;
using engine::Install::validate_game_match;

TEST_CASE("game_match identical ids match", "[game_match]") {
  auto r = validate_game_match("skyrimspecialedition", "skyrimspecialedition");
  CHECK(r.matches);
  CHECK(r.error.empty());
}

TEST_CASE("game_match different games mismatch with error", "[game_match]") {
  auto r = validate_game_match("skyrimspecialedition", "fallout4");
  CHECK_FALSE(r.matches);
  CHECK_FALSE(r.error.empty());
  // Error names both sides so the user knows what went wrong.
  CHECK(r.error.find("skyrimspecialedition") != std::string::npos);
  CHECK(r.error.find("fallout4") != std::string::npos);
}

TEST_CASE("game_match empty pack game blocks", "[game_match]") {
  auto r = validate_game_match("", "fallout4");
  CHECK_FALSE(r.matches);
  CHECK_FALSE(r.error.empty());
}

TEST_CASE("game_match empty instance game blocks", "[game_match]") {
  auto r = validate_game_match("skyrimspecialedition", "");
  CHECK_FALSE(r.matches);
  CHECK_FALSE(r.error.empty());
}

TEST_CASE("game_match both empty blocks", "[game_match]") {
  auto r = validate_game_match("", "");
  CHECK_FALSE(r.matches);
  CHECK_FALSE(r.error.empty());
}

TEST_CASE("game_match whitespace-only ids block", "[game_match]") {
  CHECK_FALSE(validate_game_match("   ", "fallout4").matches);
  CHECK_FALSE(validate_game_match("skyrimspecialedition", "  ").matches);
}

TEST_CASE("game_match normalization is case-insensitive", "[game_match]") {
  CHECK(validate_game_match("SkyrimSpecialEdition", "skyrimspecialedition").matches);
  CHECK(normalize_game_id("FALLOUT4") == "fallout4");
}

TEST_CASE("game_match normalization trims whitespace", "[game_match]") {
  CHECK(
      validate_game_match("  skyrimspecialedition  ", "skyrimspecialedition").matches);
}

TEST_CASE("game_match aliases resolve to canonical id", "[game_match]") {
  CHECK(normalize_game_id("skyrimse") == "skyrimspecialedition");
  CHECK(normalize_game_id("newvegas") == "falloutnv");
  CHECK(normalize_game_id("falloutnewvegas") == "falloutnv");
  CHECK(normalize_game_id("enderalspecialedition") == "enderalse");
  CHECK(validate_game_match("skyrimse", "SkyrimSpecialEdition").matches);
  CHECK(validate_game_match("newvegas", "falloutnv").matches);
}

TEST_CASE("game_match unknown ids compare literally", "[game_match]") {
  // A future game not in the alias table still matches itself ...
  CHECK(validate_game_match("mynewgame", "MyNewGame").matches);
  // ... but not a different game.
  CHECK_FALSE(validate_game_match("mynewgame", "othergame").matches);
}

TEST_CASE("game_match skyrim LE vs SE are different games", "[game_match]") {
  CHECK_FALSE(validate_game_match("skyrim", "skyrimspecialedition").matches);
}
