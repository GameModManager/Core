// Workspace-6su: filter_existing_executables() — plugin-declared executable
// names are kept only when they physically exist under game_dir. The scan is
// the platform filter: a Windows .exe declared in the CSV simply does not
// exist on a macOS game dir and is dropped, while a ".app" bundle directory
// counts as found. Free function (fs_utils.h) so it tests without a UI.
#include "engine/core/util/fs_utils.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {
void touch(const std::filesystem::path& p) {
    std::ofstream f(p);
    f << "x";
}
}  // namespace

TEST_CASE("filter_existing_executables", "[engine]") {
    auto dir = std::filesystem::temp_directory_path() / "gmm6su_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "REPENTOGONLauncher");
    touch(dir / "isaac-ng.exe");
    touch(dir / "REPENTOGONLauncher" / "REPENTOGONLauncher.exe");
    std::filesystem::create_directory(dir / "The Binding of Isaac Rebirth.app");

    // Missing names dropped, subdir candidate kept, declaration order intact.
    auto out = engine::filter_existing_executables(
        dir,
        "REPENTOGONLauncher/REPENTOGONLauncher.exe, missing.exe ,"
        "isaac-ng.exe");
    REQUIRE(out.size() == 2);
    CHECK(out[0] == "REPENTOGONLauncher/REPENTOGONLauncher.exe");
    CHECK(out[1] == "isaac-ng.exe");

    // A ".app" directory counts as found (macOS bundle).
    out = engine::filter_existing_executables(
        dir, "The Binding of Isaac Rebirth.app");
    REQUIRE(out.size() == 1);
    CHECK(out[0] == "The Binding of Isaac Rebirth.app");

    // A plain directory without the ".app" suffix does not.
    std::filesystem::create_directory(dir / "some_dir");
    CHECK(engine::filter_existing_executables(dir, "some_dir").empty());

    // Nothing exists -> empty; empty CSV -> empty; empty game_dir -> empty.
    CHECK(engine::filter_existing_executables(dir, "nope.exe").empty());
    CHECK(engine::filter_existing_executables(dir, "").empty());
    CHECK(engine::filter_existing_executables({}, "isaac-ng.exe").empty());

    std::filesystem::remove_all(dir);
}
