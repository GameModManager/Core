#include "gmm_swift_bridge.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

std::string read_text(const fs::path& path) {
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

fs::path fixture_root() {
    const auto root = fs::temp_directory_path() /
                      ("gmm_swift_bridge_" + std::to_string(getpid()));
    fs::remove_all(root);
    fs::create_directories(root / "instances" / "Fixture" / "profiles" / "Default");
    fs::create_directories(root / "instances" / "Fixture" / "mods" / "Alpha" / "meshes");
    fs::create_directories(root / "instances" / "Fixture" / "mods" / "Beta" / "meshes");

    std::ofstream(root / "instances" / "Fixture" / "instance.toml")
        << "game_id = \"SkyrimSpecialEdition\"\n";
    std::ofstream(root / "instances" / "Fixture" / "profiles" / "Default" / "modlist.txt")
        << "+Beta\n-Alpha\n";
    return root;
}

}  // namespace

TEST_CASE("Swift bridge handles nulls, cancellation, and owned errors", "[macos][swiftui]") {
    auto* operation = gmm_swift_operation_create();
    REQUIRE(operation != nullptr);
    REQUIRE(gmm_swift_operation_is_cancelled(operation) == 0);
    gmm_swift_operation_cancel(operation);
    REQUIRE(gmm_swift_operation_is_cancelled(operation) == 1);
    REQUIRE(gmm_swift_snapshot_create(nullptr, "missing", operation) == nullptr);
    gmm_swift_operation_destroy(operation);

    REQUIRE(gmm_swift_instance_count(nullptr) == 0);
    REQUIRE(gmm_swift_instance_id(nullptr, 0) == nullptr);
    REQUIRE(gmm_swift_snapshot_instance_id(nullptr) == nullptr);
    REQUIRE(gmm_swift_snapshot_mod_count(nullptr) == 0);
    REQUIRE(gmm_swift_snapshot_mod_at(nullptr, 0).id == nullptr);
    REQUIRE(gmm_swift_last_error(nullptr) == nullptr);
    REQUIRE(gmm_swift_subscribe_refresh(nullptr, nullptr, nullptr) == 0);
    gmm_swift_engine_destroy(nullptr);
    gmm_swift_snapshot_destroy(nullptr);
    gmm_swift_operation_destroy(nullptr);

    auto* engine = gmm_swift_engine_create("/path/that/does/not/exist", nullptr);
    REQUIRE(engine != nullptr);
    REQUIRE(gmm_swift_snapshot_create(engine, "missing", nullptr) == nullptr);
    const char* error = gmm_swift_last_error(engine);
    REQUIRE(error != nullptr);
    REQUIRE(std::string(error).find("instance was not found") != std::string::npos);
    gmm_swift_free_string(error);
    gmm_swift_engine_destroy(engine);
}

TEST_CASE("Swift bridge scans a fixture without writing it", "[macos][swiftui]") {
    const auto root = fixture_root();
    const auto instance_toml = read_text(root / "instances" / "Fixture" / "instance.toml");
    const auto modlist = read_text(
        root / "instances" / "Fixture" / "profiles" / "Default" / "modlist.txt");
    auto* engine = gmm_swift_engine_create(
        (root / "instances").c_str(), GMM_SWIFT_TEST_PLUGINS_DIR);
    REQUIRE(engine != nullptr);
    REQUIRE(gmm_swift_instance_count(engine) == 1);
    REQUIRE(std::string(gmm_swift_instance_id(engine, 0)) == "Fixture");

    auto* snapshot = gmm_swift_snapshot_create(engine, "Fixture", nullptr);
    REQUIRE(snapshot != nullptr);
    REQUIRE(std::string(gmm_swift_snapshot_instance_id(snapshot)) == "Fixture");
    REQUIRE(std::string(gmm_swift_snapshot_game_id(snapshot)) == "SkyrimSpecialEdition");
    REQUIRE(std::string(gmm_swift_snapshot_profile_id(snapshot)) == "Default");
    REQUIRE(gmm_swift_snapshot_mod_count(snapshot) == 2);
    const auto first = gmm_swift_snapshot_mod_at(snapshot, 0);
    const auto second = gmm_swift_snapshot_mod_at(snapshot, 1);
    REQUIRE(std::string(first.id) == "Alpha");
    REQUIRE(first.order == 0);
    REQUIRE(first.enabled == 0);
    REQUIRE(std::string(second.id) == "Beta");
    REQUIRE(second.order == 1);
    REQUIRE(second.enabled == 1);
    gmm_swift_snapshot_destroy(snapshot);
    gmm_swift_engine_destroy(engine);
    REQUIRE(read_text(root / "instances" / "Fixture" / "instance.toml") == instance_toml);
    REQUIRE(read_text(root / "instances" / "Fixture" / "profiles" / "Default" / "modlist.txt") ==
            modlist);
    fs::remove_all(root);
}
