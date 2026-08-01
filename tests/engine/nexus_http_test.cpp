// Regression test for engine::encode_url_path — libcurl rejects URLs with raw
// spaces, and Nexus CDN download URLs embed the archive filename unencoded.
#include "engine/source/nexus_http.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void expect_enc(const std::string& in, const std::string& expected) {
    const std::string got = engine::encode_url_path(in);
    if (got != expected) {
        std::cerr << "FAIL: encode_url_path(" << in << ")\n"
                  << "  expected [" << expected << "]\n"
                  << "  got      [" << got << "]\n";
        ++failures;
    }
}

}  // namespace

int main() {
    // The real failure: Nexus CDN URL with unencoded spaces in the filename.
    expect_enc(
        "https://files.nexus-cdn.com/1704/19080/RaceMenu Anniversary Edition "
        "v0-4-20-0-19080-0-4-20-0-1776620918.7z"
        "?expires=1785618062&md5=X_HVDRrSzTzo7lHDr3eGCA&user_id=44196692",
        "https://files.nexus-cdn.com/1704/19080/RaceMenu%20Anniversary%20Edition%20"
        "v0-4-20-0-19080-0-4-20-0-1776620918.7z"
        "?expires=1785618062&md5=X_HVDRrSzTzo7lHDr3eGCA&user_id=44196692");

    // Already-encoded escapes are preserved (no double-encoding).
    expect_enc("https://host/dir%20a/file%2Bx.7z?token=abc",
               "https://host/dir%20a/file%2Bx.7z?token=abc");

    // Query string is untouched, path encoding still applies.
    expect_enc("https://host/a b.zip?a b=1",
               "https://host/a%20b.zip?a b=1");

    // No path / no scheme / bare host: left as-is.
    expect_enc("https://host", "https://host");
    expect_enc("https://host/", "https://host/");
    expect_enc("host/path with space", "host/path with space");

    // Other unsafe path bytes are encoded too.
    expect_enc("https://host/a(b)c#x.7z", "https://host/a%28b%29c%23x.7z");

    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "nexus_http_test: all PASS\n";
    return EXIT_SUCCESS;
}
