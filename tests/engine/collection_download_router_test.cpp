// Test for engine::Collection download path routing.
#include "engine/collection/download_router.h"

#include <catch2/catch_test_macros.hpp>

namespace {

using namespace engine::Collection;

void expect_path(const ModSource &source, const AccountStatus &account,
                 DownloadPath want) {
  const RouteOutcome out = route_download(source, account);
  INFO(out.reason);
  REQUIRE(out.path == want);
  REQUIRE(!out.reason.empty());
}

SourceNexus nexus_source() {
  SourceNexus s;
  s.resolution  = SourceResolution::Api;
  s.game_domain = "skyrimspecialedition";
  s.mod_id      = 184625;
  s.file_id     = 781833;
  return s;
}

}  // namespace

TEST_CASE("nexus premium routes to auto download", "[engine]") {
  AccountStatus premium;
  premium.authenticated     = true;
  premium.can_auto_download = true;
  expect_path(ModSource{nexus_source()}, premium, DownloadPath::Auto);
}

TEST_CASE("nexus free account routes to browser flow", "[engine]") {
  AccountStatus free;
  free.authenticated = true;  // API key present, but no premium tier
  expect_path(ModSource{nexus_source()}, free, DownloadPath::Browser);
}

TEST_CASE("nexus anonymous routes to browser flow", "[engine]") {
  expect_path(ModSource{nexus_source()}, AccountStatus{}, DownloadPath::Browser);
}

TEST_CASE("browser-declared entry stays on browser even for premium", "[engine]") {
  SourceLoversLab s;  // default resolution is Browser
  AccountStatus auth;
  auth.authenticated = true;
  expect_path(ModSource{s}, auth, DownloadPath::Browser);
}

TEST_CASE("loverslab session cookie routes api entry to auto", "[engine]") {
  SourceLoversLab s;
  s.resolution = SourceResolution::Api;
  AccountStatus auth;
  auth.authenticated = true;
  expect_path(ModSource{s}, auth, DownloadPath::Auto);
  expect_path(ModSource{s}, AccountStatus{}, DownloadPath::Browser);
}

TEST_CASE("steam workshop routes to external client", "[engine]") {
  SourceSteamWorkshop s;
  AccountStatus premium;
  premium.authenticated     = true;
  premium.can_auto_download = true;
  expect_path(ModSource{s}, premium, DownloadPath::ExternalClient);
}

TEST_CASE("direct url auto-downloads anonymously", "[engine]") {
  SourceDirect s;
  s.resolution = SourceResolution::Api;
  s.url        = "https://example.com/mod.7z";
  expect_path(ModSource{s}, AccountStatus{}, DownloadPath::Auto);
}

TEST_CASE("capability table is source-driven, not hardcoded per call", "[engine]") {
  const auto nexus = capabilities_for("nexus");
  REQUIRE(nexus.api_download);
  REQUIRE(nexus.needs_auth);
  REQUIRE(nexus.needs_premium);

  const auto direct = capabilities_for("direct");
  REQUIRE(direct.api_download);
  REQUIRE(!direct.needs_auth);
  REQUIRE(!direct.needs_premium);

  const auto steam = capabilities_for("steam_workshop");
  REQUIRE(steam.external_client);
  REQUIRE(!steam.api_download);

  // Unknown future source: permissive default, declared resolution decides.
  const auto unknown = capabilities_for("some_future_source");
  REQUIRE(unknown.api_download);
  const RouteOutcome out =
      route_download(SourceResolution::Api, AccountStatus{}, unknown);
  REQUIRE(out.path == DownloadPath::Auto);
}
