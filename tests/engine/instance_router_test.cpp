// Tests for engine::Install instance routing: matching game prompts
// append-or-new, no instance or a mismatched game routes to creation, and
// the pe40 append seam stays blocked (but game-gated).
#include "engine/install/instance_router.h"

#include <catch2/catch_test_macros.hpp>

using engine::Install::InstallRoute;
using engine::Install::plan_append;
using engine::Install::route_pack_install;

TEST_CASE("instance_router match prompts append-or-new", "[instance_router]") {
  auto d = route_pack_install("skyrimspecialedition", true, "skyrimspecialedition");
  CHECK(d.route == InstallRoute::PromptAppendOrNew);
  CHECK(d.notice.empty());
}

TEST_CASE("instance_router match works through aliases", "[instance_router]") {
  auto d = route_pack_install("skyrimse", true, "SkyrimSpecialEdition");
  CHECK(d.route == InstallRoute::PromptAppendOrNew);
  CHECK(d.notice.empty());
}

TEST_CASE("instance_router no instance creates new", "[instance_router]") {
  auto d = route_pack_install("skyrimspecialedition", false, "");
  CHECK(d.route == InstallRoute::CreateNewInstance);
  CHECK(d.notice.empty());
}

TEST_CASE("instance_router mismatch creates new with reason", "[instance_router]") {
  auto d = route_pack_install("skyrimspecialedition", true, "fallout4");
  CHECK(d.route == InstallRoute::CreateNewInstance);
  // The wizard must explain the redirect - never silently drop the error.
  CHECK_FALSE(d.notice.empty());
  CHECK(d.notice.find("skyrimspecialedition") != std::string::npos);
  CHECK(d.notice.find("fallout4") != std::string::npos);
}

TEST_CASE("instance_router undeclared pack game creates new with reason",
          "[instance_router]") {
  auto d = route_pack_install("", true, "fallout4");
  CHECK(d.route == InstallRoute::CreateNewInstance);
  CHECK_FALSE(d.notice.empty());
}

TEST_CASE("instance_router undeclared instance game creates new with reason",
          "[instance_router]") {
  auto d = route_pack_install("skyrimspecialedition", true, "");
  CHECK(d.route == InstallRoute::CreateNewInstance);
  CHECK_FALSE(d.notice.empty());
}

TEST_CASE("instance_router append gate opens on game match (pe40)",
          "[instance_router]") {
  // Matching games pass the gate - detailed planning lives in append_install ...
  auto ok = plan_append("skyrimspecialedition", "skyrimse");
  CHECK(ok.ok);
  CHECK(ok.error.empty());
  // ... while mismatched games are rejected by the match gate instead.
  auto blocked = plan_append("skyrimspecialedition", "fallout4");
  CHECK_FALSE(blocked.ok);
  CHECK(blocked.error.find("ismatch") != std::string::npos);
}
