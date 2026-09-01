// Live integration test - skipped when the fixture file is absent.
// Capture a real LoversLab mod page once with curl, then drop it at
// tests/engine/fixtures/loverslab_page.html to enable this test:
//
//   curl -s -A "GameModManager/0.1 (LoversLab Provider)" \
//     "https://www.loverslab.com/files/file/11488-the-xims-magazine/" \
//     -o tests/engine/fixtures/loverslab_page.html
//
// The fixture is not committed (guest-visible page data can shift between
// captures), and the test is opt-in by fixture presence so CI does not
// flake on absent local state.
#include "engine/source/loverslab_provider.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <catch2/catch_test_macros.hpp>

namespace {
std::string slurp(const std::filesystem::path& p) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
}

TEST_CASE("loverslab provider - real captured fixture", "[engine][live]") {
    const std::filesystem::path fixture =
        std::filesystem::path(__FILE__).parent_path() / "fixtures" /
        "loverslab_page.html";
    if (!std::filesystem::exists(fixture)) {
        WARN("no live fixture at " << fixture.string()
             << " - skipping (capture with curl to enable)");
        return;
    }
    const std::string body = slurp(fixture);
    INFO("body size: " << body.size());

    auto r = engine::LoversLabProvider::parse_mod_info(body);
    INFO("available: " << r.available);
    INFO("name: " << r.name);
    INFO("version: " << r.version);
    INFO("category: " << r.category);
    INFO("author: " << r.author);
    INFO("date_modified: " << r.date_modified);
    INFO("page_url: " << r.page_url);

    REQUIRE(r.available);
    REQUIRE(!r.name.empty());
    REQUIRE(!r.description.empty());
    REQUIRE(!r.date_modified.empty());
}