// Regression test for engine::encode_url_path — libcurl rejects URLs with raw
// spaces, and Nexus CDN download URLs embed the archive filename unencoded.
#include "engine/source/nexus_http.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_CASE("nexus http encode_url_path", "[engine]") {
    // The real failure: Nexus CDN URL with unencoded spaces in the filename.
    REQUIRE(engine::encode_url_path(
                "https://files.nexus-cdn.com/1704/19080/RaceMenu Anniversary "
                "Edition v0-4-20-0-19080-0-4-20-0-1776620918.7z"
                "?expires=1785618062&md5=X_HVDRrSzTzo7lHDr3eGCA&user_id=44196692") ==
            "https://files.nexus-cdn.com/1704/19080/RaceMenu%20Anniversary%20"
            "Edition%20v0-4-20-0-19080-0-4-20-0-1776620918.7z"
            "?expires=1785618062&md5=X_HVDRrSzTzo7lHDr3eGCA&user_id=44196692");

    // Already-encoded escapes are preserved (no double-encoding).
    REQUIRE(engine::encode_url_path("https://host/dir%20a/file%2Bx.7z?token=abc") ==
            "https://host/dir%20a/file%2Bx.7z?token=abc");

    // Query string is untouched, path encoding still applies.
    REQUIRE(engine::encode_url_path("https://host/a b.zip?a b=1") ==
            "https://host/a%20b.zip?a b=1");

    // No path / no scheme / bare host: left as-is.
    REQUIRE(engine::encode_url_path("https://host") == "https://host");
    REQUIRE(engine::encode_url_path("https://host/") == "https://host/");
    REQUIRE(engine::encode_url_path("host/path with space") ==
            "host/path with space");

    // Other unsafe path bytes are encoded too.
    REQUIRE(engine::encode_url_path("https://host/a(b)c#x.7z") ==
            "https://host/a%28b%29c%23x.7z");
}