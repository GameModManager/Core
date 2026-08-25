// Workspace-421: first-launch executable seeding must also find macOS app
// bundles living OUTSIDE game_dir (Steam can place Isaac's .app in
// ~/Applications or /Applications). seed_executable_candidates() keeps the
// game_dir hits as relative names (Workspace-6su behavior), appends
// extra-root hits as ABSOLUTE paths (every consumer resolves
// `game_dir / path`, which an absolute path bypasses), and dedupes by
// basename when both copies exist. Free function (launch_controller.h) so it
// tests without a full MainWindow; extra_roots is a parameter, so the merge
// logic is testable on any platform.
#include "ui/controllers/launch_controller.h"

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

TEST_CASE("seed_executable_candidates", "[ui]") {
    auto game = std::filesystem::temp_directory_path() / "gmm421_game";
    auto apps = std::filesystem::temp_directory_path() / "gmm421_apps";
    std::filesystem::remove_all(game);
    std::filesystem::remove_all(apps);
    std::filesystem::create_directories(game);
    std::filesystem::create_directories(
        apps / "The Binding of Isaac Rebirth.app");
    touch(game / "isaac-ng.exe");

    const std::string declared = "isaac-ng.exe,The Binding of Isaac Rebirth.app";

    // game_dir hit stays a relative name; outside-root hit becomes absolute.
    auto out = ui::seed_executable_candidates(game, declared, {apps});
    REQUIRE(out.size() == 2);
    CHECK(out[0] == "isaac-ng.exe");
    CHECK(out[1] == (apps / "The Binding of Isaac Rebirth.app").string());

    // Basename dedupe: when the bundle exists in BOTH places, the game_dir
    // copy wins and no duplicate entry is added.
    std::filesystem::create_directories(
        game / "The Binding of Isaac Rebirth.app");
    out = ui::seed_executable_candidates(game, declared, {apps});
    REQUIRE(out.size() == 2);
    CHECK(out[0] == "isaac-ng.exe");
    CHECK(out[1] == "The Binding of Isaac Rebirth.app");

    // No fallback roots -> plain Workspace-6su behavior.
    out = ui::seed_executable_candidates(game, declared, {});
    REQUIRE(out.size() == 2);
    CHECK(out[1] == "The Binding of Isaac Rebirth.app");

    // Missing names are dropped by the underlying filter (Workspace-6su).
    out = ui::seed_executable_candidates(game, "missing.exe", {apps});
    CHECK(out.empty());

    std::filesystem::remove_all(game);
    std::filesystem::remove_all(apps);
}
