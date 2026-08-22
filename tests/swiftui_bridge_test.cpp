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
    // Deploys walk files, not empty dirs — give Beta real content.
    std::ofstream(root / "instances" / "Fixture" / "mods" / "Beta" / "meshes" / "scene.nif")
        << "stub\n";
    const auto game_dir = root / "instances" / "Fixture" / "game";
    fs::create_directories(game_dir);

    std::ofstream(root / "instances" / "Fixture" / "instance.toml")
        << "game_id = \"SkyrimSpecialEdition\"\n"
        << "game_dir = \"" << game_dir.string() << "\"\n"
        << "steam_appid = 72850\n"
        << "executables = [{path = \"StubGame\"}]\n";
    std::ofstream(root / "instances" / "Fixture" / "profiles" / "Default" / "modlist.txt")
        << "+Beta\n-Alpha\n";

    // Harmless native stub the launch path can exec.
    const auto stub = game_dir / "StubGame";
    {
        std::ofstream out(stub, std::ios::binary);
        out << "#!/bin/sh\nexit 0\n";
    }
    fs::permissions(stub, fs::perms::owner_exec | fs::perms::owner_read |
                              fs::perms::owner_write);
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

TEST_CASE("Swift bridge scans and mutates a fixture", "[macos][swiftui]") {
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

    auto* mutation = gmm_swift_set_mod_enabled(
        engine, "Fixture", "Default", "Beta", 0, nullptr);
    REQUIRE(mutation != nullptr);
    REQUIRE(gmm_swift_result_code(mutation) == GMM_SWIFT_RESULT_OK);
    auto* updated = gmm_swift_result_snapshot(mutation);
    REQUIRE(updated != nullptr);
    REQUIRE(gmm_swift_snapshot_mod_at(updated, 1).enabled == 0);
    REQUIRE(read_text(root / "instances" / "Fixture" / "profiles" / "Default" / "modlist.txt") ==
            "# This file was automatically generated by GameModManager.\r\n"
            "-Beta\r\n"
            "-Alpha\r\n");
    gmm_swift_result_destroy(mutation);
    fs::remove_all(root);
}

TEST_CASE("Swift bridge reorders mods and reports not-found", "[macos][swiftui]") {
    const auto root = fixture_root();
    const auto modlist = root / "instances" / "Fixture" / "profiles" / "Default" / "modlist.txt";
    auto* engine = gmm_swift_engine_create(
        (root / "instances").c_str(), GMM_SWIFT_TEST_PLUGINS_DIR);
    REQUIRE(engine != nullptr);

    // Round-trip: move Beta (priority 1) to the top and verify snapshot + persistence.
    auto* mutation = gmm_swift_move_mod(engine, "Fixture", "Default", "Beta", 0, nullptr);
    REQUIRE(mutation != nullptr);
    REQUIRE(gmm_swift_result_code(mutation) == GMM_SWIFT_RESULT_OK);
    auto* updated = gmm_swift_result_snapshot(mutation);
    REQUIRE(updated != nullptr);
    REQUIRE(gmm_swift_snapshot_mod_count(updated) == 2);
    REQUIRE(std::string(gmm_swift_snapshot_mod_at(updated, 0).id) == "Beta");
    REQUIRE(gmm_swift_snapshot_mod_at(updated, 0).order == 0);
    REQUIRE(std::string(gmm_swift_snapshot_mod_at(updated, 1).id) == "Alpha");
    REQUIRE(gmm_swift_snapshot_mod_at(updated, 1).order == 1);
    // Snapshot is owned by the mutation result; gmm_swift_result_destroy frees it.
    gmm_swift_result_destroy(mutation);
    REQUIRE(read_text(modlist) ==
            "# This file was automatically generated by GameModManager.\r\n"
            "-Alpha\r\n"
            "+Beta\r\n");

    // Not-found failure: explicit error code, no data corruption.
    auto* missing = gmm_swift_move_mod(engine, "Fixture", "Default", "Missing", 0, nullptr);
    REQUIRE(missing != nullptr);
    REQUIRE(gmm_swift_result_code(missing) == GMM_SWIFT_RESULT_ERROR);
    const char* message = gmm_swift_result_error(missing);
    REQUIRE(message != nullptr);
    REQUIRE(std::string(message).find("mod was not found") != std::string::npos);
    gmm_swift_result_destroy(missing);
    REQUIRE(read_text(modlist) ==
            "# This file was automatically generated by GameModManager.\r\n"
            "-Alpha\r\n"
            "+Beta\r\n");

    gmm_swift_engine_destroy(engine);
    fs::remove_all(root);
}

TEST_CASE("Swift bridge creates, renames, and deletes profiles", "[macos][swiftui]") {
    const auto root = fixture_root();
    const auto profiles = root / "instances" / "Fixture" / "profiles";
    auto* engine = gmm_swift_engine_create(
        (root / "instances").c_str(), GMM_SWIFT_TEST_PLUGINS_DIR);
    REQUIRE(engine != nullptr);

    // Create round-trip: directory with defaults exists and the refreshed
    // snapshot keeps the viewed profile selected.
    auto* created = gmm_swift_create_profile(engine, "Fixture", "Testing", "Default", nullptr);
    REQUIRE(created != nullptr);
    REQUIRE(gmm_swift_result_code(created) == GMM_SWIFT_RESULT_OK);
    auto* created_snapshot = gmm_swift_result_snapshot(created);
    REQUIRE(created_snapshot != nullptr);
    REQUIRE(gmm_swift_snapshot_profile_count(created_snapshot) == 2);
    REQUIRE(std::string(gmm_swift_snapshot_profile_at(created_snapshot, 0)) == "Default");
    REQUIRE(std::string(gmm_swift_snapshot_profile_at(created_snapshot, 1)) == "Testing");
    REQUIRE(std::string(gmm_swift_snapshot_profile_id(created_snapshot)) == "Default");
    REQUIRE(fs::exists(profiles / "Testing" / "settings.ini"));
    REQUIRE(fs::exists(profiles / "Testing" / "modlist.txt"));
    gmm_swift_result_destroy(created);

    // Duplicate create fails explicitly and leaves the existing profile alone.
    auto* duplicate = gmm_swift_create_profile(engine, "Fixture", "Testing", "Default", nullptr);
    REQUIRE(duplicate != nullptr);
    REQUIRE(gmm_swift_result_code(duplicate) == GMM_SWIFT_RESULT_ERROR);
    const char* duplicate_error = gmm_swift_result_error(duplicate);
    REQUIRE(duplicate_error != nullptr);
    REQUIRE(std::string(duplicate_error).find("already exists") != std::string::npos);
    gmm_swift_result_destroy(duplicate);

    // Empty name is rejected up front.
    auto* empty = gmm_swift_create_profile(engine, "Fixture", "", "Default", nullptr);
    REQUIRE(empty != nullptr);
    REQUIRE(gmm_swift_result_code(empty) == GMM_SWIFT_RESULT_ERROR);
    gmm_swift_result_destroy(empty);

    // Rename round-trip: directory moves and the snapshot follows the new name.
    auto* renamed =
        gmm_swift_rename_profile(engine, "Fixture", "Testing", "Renamed", "Renamed", nullptr);
    REQUIRE(renamed != nullptr);
    REQUIRE(gmm_swift_result_code(renamed) == GMM_SWIFT_RESULT_OK);
    auto* renamed_snapshot = gmm_swift_result_snapshot(renamed);
    REQUIRE(renamed_snapshot != nullptr);
    REQUIRE(std::string(gmm_swift_snapshot_profile_id(renamed_snapshot)) == "Renamed");
    REQUIRE(gmm_swift_snapshot_profile_count(renamed_snapshot) == 2);
    REQUIRE(fs::exists(profiles / "Renamed" / "settings.ini"));
    REQUIRE(!fs::exists(profiles / "Testing"));
    gmm_swift_result_destroy(renamed);

    // Deleting the active profile is refused.
    auto* active = gmm_swift_delete_profile(engine, "Fixture", "Default", 1, "Default", nullptr);
    REQUIRE(active != nullptr);
    REQUIRE(gmm_swift_result_code(active) == GMM_SWIFT_RESULT_ERROR);
    const char* active_error = gmm_swift_result_error(active);
    REQUIRE(active_error != nullptr);
    REQUIRE(std::string(active_error).find("cannot be deleted") != std::string::npos);
    gmm_swift_result_destroy(active);
    REQUIRE(fs::exists(profiles / "Default"));

    // Missing profile reports not-found instead of corrupting state.
    auto* missing = gmm_swift_delete_profile(engine, "Fixture", "Ghost", 0, "Default", nullptr);
    REQUIRE(missing != nullptr);
    REQUIRE(gmm_swift_result_code(missing) == GMM_SWIFT_RESULT_ERROR);
    const char* missing_error = gmm_swift_result_error(missing);
    REQUIRE(missing_error != nullptr);
    REQUIRE(std::string(missing_error).find("does not exist") != std::string::npos);
    gmm_swift_result_destroy(missing);

    // Cancelled before the mutation runs: nothing is created or deleted.
    auto* cancelled_operation = gmm_swift_operation_create();
    gmm_swift_operation_cancel(cancelled_operation);
    auto* cancelled =
        gmm_swift_create_profile(engine, "Fixture", "Never", "Default", cancelled_operation);
    REQUIRE(cancelled != nullptr);
    REQUIRE(gmm_swift_result_code(cancelled) == GMM_SWIFT_RESULT_CANCELLED);
    gmm_swift_result_destroy(cancelled);
    gmm_swift_operation_destroy(cancelled_operation);

    // Delete round-trip back to a single profile; Default is untouched.
    auto* deleted = gmm_swift_delete_profile(engine, "Fixture", "Renamed", 0, "Default", nullptr);
    REQUIRE(deleted != nullptr);
    REQUIRE(gmm_swift_result_code(deleted) == GMM_SWIFT_RESULT_OK);
    auto* deleted_snapshot = gmm_swift_result_snapshot(deleted);
    REQUIRE(deleted_snapshot != nullptr);
    REQUIRE(gmm_swift_snapshot_profile_count(deleted_snapshot) == 1);
    REQUIRE(std::string(gmm_swift_snapshot_profile_id(deleted_snapshot)) == "Default");
    gmm_swift_result_destroy(deleted);
    REQUIRE(!fs::exists(profiles / "Renamed"));
    // The surviving profile's modlist is never touched by lifecycle ops.
    REQUIRE(read_text(profiles / "Default" / "modlist.txt") == "+Beta\n-Alpha\n");

    gmm_swift_engine_destroy(engine);
    fs::remove_all(root);
}

TEST_CASE("Swift bridge launches through prepare_launch_params", "[macos][swiftui]") {
    const auto root = fixture_root();
    const auto game_dir = root / "instances" / "Fixture" / "game";
    const auto stub = (game_dir / "StubGame").string();
    auto* engine = gmm_swift_engine_create(
        (root / "instances").c_str(), GMM_SWIFT_TEST_PLUGINS_DIR);
    REQUIRE(engine != nullptr);

    // Snapshot exposes the launch-relevant instance fields.
    auto* snapshot = gmm_swift_snapshot_create(engine, "Fixture", nullptr);
    REQUIRE(snapshot != nullptr);
    REQUIRE(std::string(gmm_swift_snapshot_game_dir(snapshot)) == game_dir.string());
    REQUIRE(gmm_swift_snapshot_steam_appid(snapshot) == 72850);
    REQUIRE(gmm_swift_snapshot_executable_count(snapshot) == 1);
    REQUIRE(std::string(gmm_swift_snapshot_executable_at(snapshot, 0)) == "StubGame");
    gmm_swift_snapshot_destroy(snapshot);
    REQUIRE(!fs::exists(game_dir / "meshes"));

    // Launch round-trip: enabled mod Beta is deployed into the game dir and
    // the stub process starts.
    auto* launch = gmm_swift_launch(engine, "Fixture", stub.c_str(), nullptr);
    REQUIRE(launch != nullptr);
    REQUIRE(gmm_swift_launch_code(launch) == GMM_SWIFT_RESULT_OK);
    REQUIRE(gmm_swift_launch_pid(launch) > 0);
    REQUIRE(gmm_swift_launch_overlay(launch) == 0);  // OverlayFS is Linux-only
    gmm_swift_launch_destroy(launch);
    // Beta (enabled) deployed into the game dir's deploy prefix (Skyrim:
    // Data/), Alpha (disabled in modlist... folder sentinel governs deploy).
    REQUIRE(fs::exists(game_dir / "Data" / "meshes" / "scene.nif"));
    // The stub itself is untouched by the deploy.
    REQUIRE(fs::exists(stub));

    // Empty executable is rejected before any engine work.
    auto* empty = gmm_swift_launch(engine, "Fixture", "", nullptr);
    REQUIRE(empty != nullptr);
    REQUIRE(gmm_swift_launch_code(empty) == GMM_SWIFT_RESULT_ERROR);
    const char* empty_error = gmm_swift_launch_error(empty);
    REQUIRE(empty_error != nullptr);
    REQUIRE(std::string(empty_error).find("executable is required") != std::string::npos);
    gmm_swift_launch_destroy(empty);

    // Cancelled before launch: no new process, explicit cancelled code.
    auto* operation = gmm_swift_operation_create();
    gmm_swift_operation_cancel(operation);
    auto* cancelled = gmm_swift_launch(engine, "Fixture", stub.c_str(), operation);
    REQUIRE(cancelled != nullptr);
    REQUIRE(gmm_swift_launch_code(cancelled) == GMM_SWIFT_RESULT_CANCELLED);
    gmm_swift_launch_destroy(cancelled);
    gmm_swift_operation_destroy(operation);

    // Missing game dir reports a clean error instead of launching.
    auto* missing = gmm_swift_launch(engine, "Fixture", "/nonexistent/exe", nullptr);
    REQUIRE(missing != nullptr);
    REQUIRE(gmm_swift_launch_code(missing) == GMM_SWIFT_RESULT_ERROR);
    gmm_swift_launch_destroy(missing);

    // Process liveness probe: self is alive, nonsense pid is not.
    REQUIRE(gmm_swift_process_alive(getpid()) == 1);
    REQUIRE(gmm_swift_process_alive(-1) == 0);
    REQUIRE(gmm_swift_process_alive(0) == 0);

    gmm_swift_engine_destroy(engine);
    fs::remove_all(root);
}

TEST_CASE("Swift bridge creates separators and applies imported modlists", "[macos][swiftui]") {
    const auto root = fixture_root();
    const auto mods_dir = root / "instances" / "Fixture" / "mods";
    auto* engine = gmm_swift_engine_create(
        (root / "instances").c_str(), GMM_SWIFT_TEST_PLUGINS_DIR);
    REQUIRE(engine != nullptr);

    // Separator round-trip: folder with suffix appears in the mods directory.
    // Created AFTER the modlist assertions below — a later apply would pick
    // it up as a new managed entry (engine behavior, MO2 parity).
    const auto modlist_path = root / "instances" / "Fixture" / "profiles" / "Default" / "modlist.txt";

    // Apply an imported order: Beta first, Alpha second. Unknown entries are
    // skipped; enabled states stay untouched.
    const char* order[] = {"Beta", "Ghost", "Alpha"};
    auto* applied = gmm_swift_apply_modlist(engine, "Fixture", "Default", order, 3, nullptr);
    REQUIRE(applied != nullptr);
    REQUIRE(gmm_swift_result_code(applied) == GMM_SWIFT_RESULT_OK);
    auto* snapshot = gmm_swift_result_snapshot(applied);
    REQUIRE(snapshot != nullptr);
    REQUIRE(std::string(gmm_swift_snapshot_mod_at(snapshot, 0).id) == "Beta");
    REQUIRE(std::string(gmm_swift_snapshot_mod_at(snapshot, 1).id) == "Alpha");
    REQUIRE(gmm_swift_snapshot_mod_at(snapshot, 0).enabled == 1);
    REQUIRE(gmm_swift_snapshot_mod_at(snapshot, 1).enabled == 0);
    gmm_swift_result_destroy(applied);
    // modlist.txt writes highest priority first: Alpha(1), Beta(0).
    REQUIRE(read_text(modlist_path) ==
            "# This file was automatically generated by GameModManager.\r\n"
            "-Alpha\r\n"
            "+Beta\r\n");

    // All-unknown list is an explicit error, no corruption.
    const char* unknown[] = {"Missing"};
    auto* missing = gmm_swift_apply_modlist(engine, "Fixture", "Default", unknown, 1, nullptr);
    REQUIRE(missing != nullptr);
    REQUIRE(gmm_swift_result_code(missing) == GMM_SWIFT_RESULT_ERROR);
    gmm_swift_result_destroy(missing);

    // Snapshot exposes the instance paths for the open-folder buttons.
    auto* paths = gmm_swift_snapshot_create(engine, "Fixture", nullptr);
    REQUIRE(paths != nullptr);
    REQUIRE(std::string(gmm_swift_snapshot_instance_root(paths)) ==
            (root / "instances" / "Fixture").string());
    gmm_swift_snapshot_destroy(paths);

    auto* separator = gmm_swift_create_separator(engine, "Fixture", "Weapons", nullptr);
    REQUIRE(separator != nullptr);
    REQUIRE(gmm_swift_result_code(separator) == GMM_SWIFT_RESULT_OK);
    gmm_swift_result_destroy(separator);
    REQUIRE(fs::exists(mods_dir / "Weapons_separator"));
    // Creating a separator never rewrites profiles.
    REQUIRE(read_text(modlist_path) ==
            "# This file was automatically generated by GameModManager.\r\n"
            "-Alpha\r\n"
            "+Beta\r\n");

    // Duplicate name is refused.
    auto* duplicate = gmm_swift_create_separator(engine, "Fixture", "Weapons", nullptr);
    REQUIRE(duplicate != nullptr);
    REQUIRE(gmm_swift_result_code(duplicate) == GMM_SWIFT_RESULT_ERROR);
    const char* duplicate_error = gmm_swift_result_error(duplicate);
    REQUIRE(duplicate_error != nullptr);
    REQUIRE(std::string(duplicate_error).find("already exists") != std::string::npos);
    gmm_swift_result_destroy(duplicate);

    gmm_swift_engine_destroy(engine);
    fs::remove_all(root);
}
