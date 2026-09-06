// Test for engine::NxmRouter::parse + Source::Router::parse_modl.
#include "engine/source/nxm/nxm_router.h"
#include "engine/source/router.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace {

void expect_link(const std::string& url, const std::string& domain, long long mod,
                 long long file, const std::string& key = "", long long expire = 0) {
    const auto link = engine::NxmRouter::parse(url);
    INFO(url);
    REQUIRE(link.nexus_domain == domain);
    REQUIRE(link.mod_id == mod);
    REQUIRE(link.file_id == file);
    REQUIRE(link.key == key);
    REQUIRE(link.expire == expire);
}

void expect_modl(const std::string& url, const std::string& game,
                 const std::string& direct) {
    const auto link = engine::Source::Router::parse_modl(url);
    INFO(url);
    REQUIRE(link.valid());
    REQUIRE(link.game_id == game);
    REQUIRE(link.direct_url == direct);
}

}  // namespace

TEST_CASE("nxm router parse", "[engine]") {
    // Real Nexus-site form:
    //   nxm://<game-domain>/mods/<mod>/files/<file>?key=...&expires=...&user_id=...
    expect_link("nxm://skyrimspecialedition/mods/184625/files/781833?key=WPsTiCS-cJMsRv29vXJX4g&expires=1785695383&user_id=44196692",
                "skyrimspecialedition", 184625, 781833,
                "WPsTiCS-cJMsRv29vXJX4g", 1785695383);
    expect_link("nxm://thebindingofisaacrebirth/mods/68/files/528",
                "thebindingofisaacrebirth", 68, 528);

    // Empty-authority mangling: nxm:///<game>/... (browser/portal layers).
    expect_link("nxm:///thebindingofisaacrebirth/mods/68/files/528",
                "thebindingofisaacrebirth", 68, 528);
    expect_link("nxm:///skyrimspecialedition/mods/123/files/456?key=abc&expire=1",
                "skyrimspecialedition", 123, 456, "abc", 1);

    // Modern form: nxm://<game-domain>/mods/<mod>/files/<file>
    expect_link("nxm://thebindingofisaacrebirth/mods/68/files/528",
                "thebindingofisaacrebirth", 68, 528);
    expect_link("nxm://skyrimspecialedition/mods/123/files/456?key=abc&expire=1",
                "skyrimspecialedition", 123, 456, "abc", 1);

    // Nexus-site form: nxm://nexus/<game-domain>/mods/<mod>/files/<file>
    expect_link("nxm://nexus/thebindingofisaacrebirth/mods/68/files/528",
                "thebindingofisaacrebirth", 68, 528);
    expect_link("nxm://nexus/skyrimspecialedition/mods/123/files/456&key=zz&expire=2",
                "skyrimspecialedition", 123, 456, "zz", 2);

    // Legacy NMM form (no game domain): domain stays "nexus", ids still parsed.
    const auto legacy = engine::NxmRouter::parse("nxm://nexus/mods/68?key=abc&expire=1");
    REQUIRE(legacy.nexus_domain == "nexus");
    REQUIRE(legacy.mod_id == 68);
    REQUIRE(legacy.file_id == 0);
}

TEST_CASE("modl router parse", "[engine]") {
    // Canonical form: modl://<host>/?url=<percent-encoded https URL>
    expect_modl("modl://falloutnv/?url=https%3A%2F%2Fcdn.mod.pub%2Ffile.7z",
                "falloutnv", "https://cdn.mod.pub/file.7z");
    expect_modl(
        "modl://skyrimse/?url=https%3A%2F%2Fexample.com%2Fpath%2FMod%20Name.7z",
        "skyrimse", "https://example.com/path/Mod Name.7z");

    // MO2 alias normalization (case-insensitive on input).
    expect_modl("modl://FalloutNV/?url=https%3A%2F%2Fa.b%2Fc",
                "falloutnv", "https://a.b/c");
    expect_modl("modl://SkyrimSpecialEdition/?url=https%3A%2F%2Fa.b%2Fc",
                "skyrimse", "https://a.b/c");
    expect_modl("modl://newvegas/?url=https%3A%2F%2Fa.b%2Fc",
                "falloutnv", "https://a.b/c");
    expect_modl("modl://other/?url=https%3A%2F%2Fa.b%2Fc",
                "other", "https://a.b/c");

    // Triple-slash tolerance (browser/portal layers).
    expect_modl("modl:///falloutnv/?url=https%3A%2F%2Fa.b%2Fc",
                "falloutnv", "https://a.b/c");

    // "other" + starfield present in aliases.
    expect_modl("modl://starfield/?url=https%3A%2F%2Fa.b%2Fc",
                "starfield", "https://a.b/c");

    // Malformed: empty host, missing url param, non-https scheme, wrong scheme.
    const auto no_url = engine::Source::Router::parse_modl("modl://falloutnv/");
    REQUIRE_FALSE(no_url.valid());

    const auto wrong_scheme = engine::Source::Router::parse_modl(
        "modl://falloutnv/?url=http%3A%2F%2Fa.b%2Fc");
    REQUIRE_FALSE(wrong_scheme.valid());

    const auto no_scheme = engine::Source::Router::parse_modl(
        "modl://falloutnv/?url=javascript%3Aalert(1)");
    REQUIRE_FALSE(no_scheme.valid());

    const auto nxm_misparse = engine::Source::Router::parse_modl(
        "nxm://skyrimspecialedition/mods/68/files/528");
    REQUIRE_FALSE(nxm_misparse.valid());

    // Unrecognized host passes through (caller checks plugin game_id match).
    const auto unknown = engine::Source::Router::parse_modl(
        "modl://notagame/?url=https%3A%2F%2Fa.b%2Fc");
    REQUIRE(unknown.valid());
    REQUIRE(unknown.game_id == "notagame");
    REQUIRE(unknown.direct_url == "https://a.b/c");
}