// The three defects in the shared self-updater helpers, plus the endpoint
// choice that makes include_prereleases mean something.
//
// None of these can be observed through fetch_update_info, which needs a live
// release feed, so each is pinned at the function that carries the behaviour:
// the endpoint selector, the version comparison, and the asset lookup.
#include "engine/core/github.h"
#include "engine/update/self_updater_p.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using engine::GitHub;
using engine::update::find_asset_url;

namespace {

GitHub::Release release_with(std::vector<GitHub::Asset> assets) {
  GitHub::Release r;
  r.tag_name   = "v0.5.28";
  r.prerelease = false;
  r.assets     = std::move(assets);
  return r;
}

GitHub::Asset asset(const std::string &name) {
  GitHub::Asset a;
  a.name         = name;
  a.download_url = "https://example.invalid/" + name;
  return a;
}

}  // namespace

// ---------------------------------------------------------------------------
// Bug 1: include_prereleases could not change the answer, because
// /releases/latest returns a non-prerelease by definition.
// ---------------------------------------------------------------------------
TEST_CASE("self updater: include_prereleases changes which endpoint is queried",
          "[engine][update]") {
  const auto stable = GitHub::releases_endpoint("GameModManager", "GMM", false);
  const auto any    = GitHub::releases_endpoint("GameModManager", "GMM", true);

  INFO("a flag that cannot change the URL cannot change the result");
  CHECK(stable != any);
  CHECK(stable == "https://api.github.com/repos/GameModManager/GMM/releases/latest");
  CHECK(any == "https://api.github.com/repos/GameModManager/GMM/releases");
}

// ---------------------------------------------------------------------------
// Bug 2: parse_version could not parse v0.6.0-rc1, so a prerelease tag was
// unparseable and silently reported as "no update available".
// ---------------------------------------------------------------------------
TEST_CASE("self updater: a prerelease tag parses and sorts below its release",
          "[engine][update]") {
  INFO("the old full-string regex_match rejected any tag with a suffix");
  CHECK(GitHub::compare_versions("v0.6.0-rc1", "v0.6.0") < 0);
  CHECK(GitHub::compare_versions("0.6.0-rc1", "0.6.0") < 0);

  // Order among prereleases, so an rc2 is offered over an rc1.
  CHECK(GitHub::compare_versions("v0.6.0-rc2", "v0.6.0-rc1") > 0);
  CHECK(GitHub::compare_versions("v0.6.0-rc1", "v0.6.0-rc1") == 0);
  CHECK(GitHub::compare_versions("v0.6.0-beta", "v0.6.0-rc1") < 0);

  // And the plain forms still work, so nothing regressed for them.
  CHECK(GitHub::compare_versions("v0.5.27", "v0.5.28") < 0);
  CHECK(GitHub::compare_versions("v1.0.0", "v0.9.9") > 0);
  CHECK(GitHub::compare_versions("0.5.27", "0.5.27") == 0);
}

// ---------------------------------------------------------------------------
// Bug 3: find_asset_url("") matched the first asset arbitrarily, because a
// zero-length suffix compares equal to every name.
// ---------------------------------------------------------------------------
TEST_CASE("self updater: an empty asset suffix selects nothing", "[engine][update]") {
  const auto rel = release_with({
      asset("GameModManager-Setup.exe"),
      asset("GameModManager-Portable.zip"),
      asset("GameModManager.dmg"),
  });

  INFO("with three assets listed, an empty suffix must not pick the first");
  CHECK(find_asset_url(rel, "").empty());

  // A real suffix still resolves, and resolves to the asset it names.
  CHECK(find_asset_url(rel, ".exe") ==
        "https://example.invalid/GameModManager-Setup.exe");
  CHECK(find_asset_url(rel, ".dmg") == "https://example.invalid/GameModManager.dmg");
  CHECK(find_asset_url(rel, ".zip") ==
        "https://example.invalid/GameModManager-Portable.zip");

  // A suffix nothing matches is empty, not "the first one".
  CHECK(find_asset_url(rel, ".AppImage").empty());
  // An exact-length name still matches.
  CHECK(find_asset_url(release_with({asset(".run")}), ".run") ==
        "https://example.invalid/.run");
  CHECK(find_asset_url(release_with({asset("run")}), ".run").empty());
  // No assets at all.
  CHECK(find_asset_url(release_with({}), ".exe").empty());
}
