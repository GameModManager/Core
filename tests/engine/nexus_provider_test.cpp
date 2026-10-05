// Test for Nexus::Provider::parse_mod_info — the pure JSON mapping behind the
// Mod Info Nexus tab "Refresh" button (mods/{game}/mods/{id}.json).
#include "engine/source/nexus/provider.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <catch2/catch_test_macros.hpp>

namespace {
void require(bool cond, const char *msg) {
  INFO(msg);
  REQUIRE(cond);
}
}  // namespace

TEST_CASE("nexus provider", "[engine]") {
  using engine::Source::Nexus::ModInfoResult;
  using engine::Source::Nexus::Provider;

  // --- Realistic mods/{game}/mods/{id}.json response. ---
  const std::string body =
      R"({
          "name": "RaceMenu",
          "summary": "Character creation overhaul",
          "version": "0.4.20",
          "newest_version": "0.4.21",
          "available": true,
          "category_id": 21,
          "description": "[size=3][b]RaceMenu[/b][/size]\r\n A description.",
          "author": "expired6978"
        })";
  ModInfoResult r = Provider::parse_mod_info(body);
  require(r.available, "available parsed");
  require(r.name == "RaceMenu", "name parsed");
  require(r.version == "0.4.20", "version parsed");
  require(r.newest_version == "0.4.21", "newest_version parsed");
  require(r.category_id == "21", "category_id parsed");
  require(r.author == "expired6978", "author parsed");
  require(r.description.find("[size=3]") != std::string::npos,
          "BBCode description parsed verbatim");

  // --- Unavailable mod: available=false but fields still present. ---
  const std::string hidden =
      R"({"name": "Hidden Mod", "available": false, "version": "1.0"})";
  ModInfoResult h = Provider::parse_mod_info(hidden);
  require(!h.available, "unavailable flag honored");
  require(h.name == "Hidden Mod" && h.version == "1.0",
          "fields still parsed when unavailable");

  // --- Garbage / non-object input -> empty result. ---
  ModInfoResult bad = Provider::parse_mod_info("not json {");
  require(!bad.available && bad.name.empty(), "garbage yields empty result");
  ModInfoResult arr = Provider::parse_mod_info("[1,2,3]");
  require(!arr.available, "non-object JSON yields empty result");
}

// updatePolicy decides WHICH file fetch() asks Nexus for. prefer is the state
// that has a fallback: the pinned fileId while Nexus still serves it, the mod's
// newest file once that file was archived or deleted upstream.
TEST_CASE("nexus provider update policy file selection", "[engine]") {
  using engine::Source::Nexus::ModInfoResult;
  using engine::Source::Nexus::Provider;

  // --- mods/{game}/mods/{id}/files.json. File 20 is archived upstream: it is
  // still listed in "files" but missing from "available_mod_files". ---
  const std::string body = R"({
      "data": {
        "files": [
          { "id": 10, "version": "1.0" },
          { "id": 20, "version": "2.0" },
          { "id": 30, "version": "3.0" }
        ],
        "available_mod_files": [10, 30]
      }
    })";
  const auto list        = Provider::parse_file_list(body);
  require(list.ok, "file list parsed");
  require(list.available.size() == 2, "only downloadable files are available");

  // prefer: the pin survives when it is still there.
  require(Provider::select_file_id("prefer", 10, list) == 10,
          "prefer keeps a pin that is still available");
  // prefer: the pin is gone, fall back to the newest file.
  require(Provider::select_file_id("prefer", 20, list) == 30,
          "prefer falls back to the newest file when the pin is archived");
  // latest ignores the pin entirely.
  require(Provider::select_file_id("latest", 10, list) == 30,
          "latest always takes the newest file");
  // exact has no fallback to offer and does not spend a request on the list.
  require(Provider::select_file_id("exact", 20, list) == 20,
          "exact keeps its pin whatever the list says");
  require(Provider::select_file_id("exact", 0, list) == 0,
          "exact with no pin yields nothing");

  // A failed or empty list never silently resolves to some other file.
  const Provider::FileList failed = Provider::parse_file_list("not json {");
  require(!failed.ok, "garbage yields an unusable list");
  require(Provider::select_file_id("prefer", 10, failed) == 0,
          "prefer with no list reports failure instead of guessing");
  require(Provider::select_file_id("latest", 0, failed) == 0,
          "latest with no list reports failure");
}
