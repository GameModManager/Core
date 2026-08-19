// Engine test for the Profile class (engine/profile) — the profile directory
// manager: settings.ini, modlist.txt (+/-/* format, priority order),
// plugins/loadorder/lockedorder/archives, atomic writes (safe_write_file),
// and the DelayedFileWriter debounce. Uses temp dirs only, no Qt.
#include "engine/profile/delayed_file_writer.h"
#include "engine/profile/profile.h"
#include "engine/profile/safe_write_file.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

std::atomic<int> g_counter{0};

fs::path make_temp_dir(const char* tag) {
    auto dir = fs::temp_directory_path() /
               ("gmm_profile_" + std::string(tag) + "_" + std::to_string(getpid()) + "_" +
                std::to_string(g_counter.fetch_add(1)));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

std::string read_text(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void write_text(const fs::path& p, const std::string& content) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << content;
}

bool has_temp_files(const fs::path& dir) {
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().filename().string().find(".tmp") != std::string::npos) {
            return true;
        }
    }
    return false;
}

// Poll `pred` until it returns true or the timeout elapses.
bool wait_until(const std::function<bool()>& pred, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return pred();
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TEST_CASE("profile constructs from directory", "[engine]") {
    auto dir = make_temp_dir("ctor");
    engine::profile::Profile profile(dir);
    REQUIRE(profile.exists());
    REQUIRE(profile.name() == dir.filename().string());
    REQUIRE(profile.directory() == dir);
    REQUIRE(profile.settings_path() == dir / "settings.ini");
    REQUIRE(profile.modlist_path() == dir / "modlist.txt");
    REQUIRE(profile.plugins_path() == dir / "plugins.txt");
    REQUIRE(profile.loadorder_path() == dir / "loadorder.txt");
    REQUIRE(profile.lockedorder_path() == dir / "lockedorder.txt");
    REQUIRE(profile.archives_path() == dir / "archives.txt");
    REQUIRE(profile.mods().empty());
}

// ---------------------------------------------------------------------------
// settings.ini
// ---------------------------------------------------------------------------

TEST_CASE("settings.ini round-trips the three profile settings", "[engine]") {
    auto dir = make_temp_dir("settings");
    {
        engine::profile::Profile profile(dir);
        REQUIRE_FALSE(profile.local_saves());
        REQUIRE_FALSE(profile.local_settings());
        REQUIRE_FALSE(profile.automatic_archive_invalidation());

        profile.set_local_saves(true);
        profile.set_local_settings(true);
        profile.set_automatic_archive_invalidation(true);
        REQUIRE(profile.save_settings());
    }
    {
        engine::profile::Profile profile(dir);
        REQUIRE(profile.local_saves());
        REQUIRE(profile.local_settings());
        REQUIRE(profile.automatic_archive_invalidation());
    }
}

TEST_CASE("settings.ini preserves unknown keys and sections", "[engine]") {
    auto dir = make_temp_dir("settings_preserve");
    write_text(dir / "settings.ini",
               "LocalSaves=false\n"
               "CustomRootKey=hello\n"
               "[Game]\n"
               "bSomething=1\n");
    engine::profile::Profile profile(dir);
    profile.set_local_saves(true);
    REQUIRE(profile.save_settings());

    const std::string content = read_text(dir / "settings.ini");
    REQUIRE(content.find("LocalSaves=true") != std::string::npos);
    REQUIRE(content.find("CustomRootKey=hello") != std::string::npos);
    REQUIRE(content.find("[Game]") != std::string::npos);
    REQUIRE(content.find("bSomething=1") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Repair
// ---------------------------------------------------------------------------

TEST_CASE("repair creates all missing required files with defaults", "[engine]") {
    auto dir = make_temp_dir("repair_missing");
    engine::profile::Profile profile(dir, 50ms);

    const auto generated = profile.repair();
    REQUIRE(generated.size() == 3);
    REQUIRE(std::find(generated.begin(), generated.end(), "settings.ini") != generated.end());
    REQUIRE(std::find(generated.begin(), generated.end(), "modlist.txt") != generated.end());
    REQUIRE(std::find(generated.begin(), generated.end(), "archives.txt") != generated.end());

    // settings.ini carries the documented defaults.
    REQUIRE(fs::exists(dir / "settings.ini"));
    REQUIRE_FALSE(profile.local_saves());
    REQUIRE_FALSE(profile.local_settings());
    REQUIRE_FALSE(profile.automatic_archive_invalidation());
    const std::string settings = read_text(dir / "settings.ini");
    REQUIRE(settings.find("LocalSaves=false") != std::string::npos);
    REQUIRE(settings.find("LocalSettings=false") != std::string::npos);
    REQUIRE(settings.find("AutomaticArchiveInvalidation=false") != std::string::npos);

    // modlist.txt and archives.txt exist and are empty.
    REQUIRE(fs::exists(dir / "modlist.txt"));
    REQUIRE(read_text(dir / "modlist.txt").empty());
    REQUIRE(fs::exists(dir / "archives.txt"));
    REQUIRE(read_text(dir / "archives.txt").empty());
}

TEST_CASE("repair is a no-op when all required files exist", "[engine]") {
    auto dir = make_temp_dir("repair_complete");
    write_text(dir / "settings.ini", "LocalSaves=true\n");
    write_text(dir / "modlist.txt", "+ModA\r\n");
    write_text(dir / "archives.txt", "Skyrim - Textures.bsa\n");

    engine::profile::Profile profile(dir, 50ms);
    REQUIRE(profile.repair().empty());

    // Existing content is untouched.
    REQUIRE(read_text(dir / "settings.ini").find("LocalSaves=true") != std::string::npos);
    REQUIRE(read_text(dir / "modlist.txt").find("+ModA") != std::string::npos);
    REQUIRE(read_text(dir / "archives.txt").find("Skyrim - Textures.bsa") != std::string::npos);
}

TEST_CASE("repair fills only the missing files", "[engine]") {
    auto dir = make_temp_dir("repair_partial");
    write_text(dir / "modlist.txt", "+ModA\r\n");

    engine::profile::Profile profile(dir, 50ms);
    const auto generated = profile.repair();

    REQUIRE(generated.size() == 2);
    REQUIRE(std::find(generated.begin(), generated.end(), "settings.ini") != generated.end());
    REQUIRE(std::find(generated.begin(), generated.end(), "archives.txt") != generated.end());
    REQUIRE(std::find(generated.begin(), generated.end(), "modlist.txt") == generated.end());

    // The existing modlist is preserved.
    REQUIRE(read_text(dir / "modlist.txt").find("+ModA") != std::string::npos);
}

TEST_CASE("repair is idempotent", "[engine]") {
    auto dir = make_temp_dir("repair_idempotent");
    engine::profile::Profile profile(dir, 50ms);

    REQUIRE(profile.repair().size() == 3);
    REQUIRE(profile.repair().empty());  // second pass finds nothing to do
    REQUIRE(profile.repair().empty());
}

// ---------------------------------------------------------------------------
// modlist.txt parsing / writing
// ---------------------------------------------------------------------------

TEST_CASE("modlist.txt parses enabled/disabled/foreign", "[engine]") {
    auto dir = make_temp_dir("parse");
    write_text(dir / "modlist.txt",
               "# comment\r\n"
               "+ModA\r\n"
               "-ModB\r\n"
               "*DLC1\r\n"
               "\r\n"
               "BareName\r\n");
    engine::profile::Profile profile(dir, 50ms);
    profile.refresh_mod_status({});

    // File order: ModA(0), ModB(1), DLC1(2), BareName(3) -> priorities
    // inverted so the first line is the highest priority.
    const auto mods = profile.mods();
    REQUIRE(mods.size() == 4);
    REQUIRE(mods[0].mod_id == "BareName");
    REQUIRE(mods[0].enabled);
    REQUIRE_FALSE(mods[0].foreign);
    REQUIRE(mods[1].mod_id == "DLC1");
    REQUIRE(mods[1].enabled);
    REQUIRE(mods[1].foreign);
    REQUIRE(mods[2].mod_id == "ModB");
    REQUIRE_FALSE(mods[2].enabled);
    REQUIRE(mods[3].mod_id == "ModA");
    REQUIRE(mods[3].enabled);
}

TEST_CASE("modlist.txt writes in priority order (highest first)", "[engine]") {
    auto dir = make_temp_dir("write_order");
    engine::profile::Profile profile(dir, 50ms);
    profile.refresh_mod_status({"Low", "Mid", "High"});
    profile.write_modlist_now();

    const std::string content = read_text(dir / "modlist.txt");
    const auto pos_low = content.find("+Low");
    const auto pos_mid = content.find("+Mid");
    const auto pos_high = content.find("+High");
    REQUIRE(pos_low != std::string::npos);
    REQUIRE(pos_mid != std::string::npos);
    REQUIRE(pos_high != std::string::npos);
    REQUIRE(pos_high < pos_mid);
    REQUIRE(pos_mid < pos_low);
}

TEST_CASE("refresh_mod_status builds priority map from file order", "[engine]") {
    auto dir = make_temp_dir("priority");
    write_text(dir / "modlist.txt", "+Top\r\n+Middle\r\n-Bottom\r\n");
    engine::profile::Profile profile(dir, 50ms);
    profile.refresh_mod_status({"Top", "Middle", "Bottom"});

    REQUIRE(profile.priority_of("Top") == 2);      // first line = highest
    REQUIRE(profile.priority_of("Middle") == 1);
    REQUIRE(profile.priority_of("Bottom") == 0);   // last line = lowest
    REQUIRE_FALSE(profile.mods()[0].enabled);      // Bottom is disabled
}

TEST_CASE("refresh_mod_status handles mods not in file", "[engine]") {
    auto dir = make_temp_dir("refresh");
    write_text(dir / "modlist.txt", "+ModB\r\n-ModA\r\n");
    engine::profile::Profile profile(dir, 50ms);
    profile.refresh_mod_status({"ModA", "ModB", "NewMod", "DLC1"}, {"DLC1"});

    // File mods: ModB=1, ModA=0 (inverted). Not in file: DLC1 (foreign) gets
    // the lowest priority, NewMod (managed) the highest; both enabled.
    REQUIRE(profile.priority_of("DLC1") == 0);
    REQUIRE(profile.priority_of("ModA") == 1);
    REQUIRE(profile.priority_of("ModB") == 2);
    REQUIRE(profile.priority_of("NewMod") == 3);

    const auto mods = profile.mods();
    REQUIRE(mods.size() == 4);
    REQUIRE(mods[0].mod_id == "DLC1");
    REQUIRE(mods[0].foreign);
    REQUIRE(mods[0].enabled);
    REQUIRE(mods[3].mod_id == "NewMod");
    REQUIRE(mods[3].enabled);
    REQUIRE_FALSE(mods[3].foreign);
}

TEST_CASE("refresh_mod_status persists newly added mods", "[engine]") {
    auto dir = make_temp_dir("refresh_persist");
    write_text(dir / "modlist.txt", "+ModA\r\n");
    {
        engine::profile::Profile profile(dir, 30ms);
        profile.refresh_mod_status({"ModA", "NewMod"});
        REQUIRE(wait_until([&] {
            return read_text(dir / "modlist.txt").find("+NewMod") != std::string::npos;
        }, 500ms));
    }
}

TEST_CASE("set_mod_enabled toggles and writes", "[engine]") {
    auto dir = make_temp_dir("toggle");
    write_text(dir / "modlist.txt", "+ModA\r\n+ModB\r\n");
    engine::profile::Profile profile(dir, 50ms);
    profile.refresh_mod_status({"ModA", "ModB"});

    profile.set_mod_enabled("ModA", false);
    auto mods = profile.mods();
    REQUIRE_FALSE(mods[1].enabled);  // ModA (priority 1) now disabled

    profile.write_modlist_now();
    REQUIRE(read_text(dir / "modlist.txt").find("-ModA") != std::string::npos);
}

TEST_CASE("set_mod_priority reorders and renumbers", "[engine]") {
    auto dir = make_temp_dir("reorder");
    write_text(dir / "modlist.txt", "+ModA\r\n+ModB\r\n+ModC\r\n");
    engine::profile::Profile profile(dir, 50ms);
    profile.refresh_mod_status({"ModA", "ModB", "ModC"});
    REQUIRE(profile.priority_of("ModA") == 2);
    REQUIRE(profile.priority_of("ModC") == 0);

    REQUIRE(profile.set_mod_priority("ModC", 2));  // move ModC to the top
    REQUIRE(profile.priority_of("ModC") == 2);
    REQUIRE(profile.priority_of("ModA") == 1);
    REQUIRE(profile.priority_of("ModB") == 0);

    profile.write_modlist_now();
    const std::string content = read_text(dir / "modlist.txt");
    REQUIRE(content.find("+ModC") < content.find("+ModA"));
    REQUIRE(content.find("+ModA") < content.find("+ModB"));
}

// ---------------------------------------------------------------------------
// Atomic writes
// ---------------------------------------------------------------------------

TEST_CASE("safe_write_file writes atomically", "[engine]") {
    auto dir = make_temp_dir("safe");
    const auto target = dir / "file.txt";

    REQUIRE(engine::profile::safe_write_file(target, "hello"));
    REQUIRE(read_text(target) == "hello");
    REQUIRE_FALSE(has_temp_files(dir));

    REQUIRE(engine::profile::safe_write_file(target, "world"));
    REQUIRE(read_text(target) == "world");
    REQUIRE_FALSE(has_temp_files(dir));
}

TEST_CASE("atomic modlist writes leave no temp files", "[engine]") {
    auto dir = make_temp_dir("atomic");
    engine::profile::Profile profile(dir, 50ms);
    profile.refresh_mod_status({"ModA"});
    profile.write_modlist_now();
    REQUIRE(fs::exists(dir / "modlist.txt"));
    REQUIRE_FALSE(has_temp_files(dir));

    profile.set_mod_enabled("ModA", false);
    profile.write_modlist_now();
    REQUIRE_FALSE(has_temp_files(dir));
    REQUIRE(read_text(dir / "modlist.txt").find("-ModA") != std::string::npos);
}

// ---------------------------------------------------------------------------
// DelayedFileWriter
// ---------------------------------------------------------------------------

TEST_CASE("DelayedFileWriter debounces into a single write", "[engine]") {
    std::atomic<int> calls{0};
    {
        engine::profile::DelayedFileWriter writer([&] { calls.fetch_add(1); }, 40ms);
        writer.write();
        writer.write();
        writer.write();
        std::this_thread::sleep_for(15ms);
        REQUIRE(calls.load() == 0);  // still inside the debounce window
        REQUIRE(wait_until([&] { return calls.load() >= 1; }, 500ms));
        REQUIRE(calls.load() == 1);  // three write() calls collapsed into one
    }
}

TEST_CASE("DelayedFileWriter write_immediately flushes now", "[engine]") {
    std::atomic<int> calls{0};
    {
        engine::profile::DelayedFileWriter writer([&] { calls.fetch_add(1); }, 10s);
        writer.write();
        writer.write_immediately();
        REQUIRE(wait_until([&] { return calls.load() >= 1; }, 500ms));
    }
}

TEST_CASE("DelayedFileWriter cancel discards pending", "[engine]") {
    std::atomic<int> calls{0};
    {
        engine::profile::DelayedFileWriter writer([&] { calls.fetch_add(1); }, 30ms);
        writer.write();
        writer.cancel();
        std::this_thread::sleep_for(80ms);
        REQUIRE(calls.load() == 0);
    }
}

TEST_CASE("DelayedFileWriter destructor flushes pending", "[engine]") {
    std::atomic<int> calls{0};
    {
        engine::profile::DelayedFileWriter writer([&] { calls.fetch_add(1); }, 10s);
        writer.write();
    }
    REQUIRE(calls.load() == 1);
}

TEST_CASE("modlist delayed write batches changes", "[engine]") {
    auto dir = make_temp_dir("delayed");
    write_text(dir / "modlist.txt", "+ModA\r\n+ModB\r\n");
    {
        engine::profile::Profile profile(dir, 50ms);
        profile.refresh_mod_status({"ModA", "ModB"});
        profile.set_mod_enabled("ModA", false);  // schedules a delayed write
        REQUIRE(read_text(dir / "modlist.txt").find("-ModA") == std::string::npos);

        REQUIRE(wait_until([&] {
            return read_text(dir / "modlist.txt").find("-ModA") != std::string::npos;
        }, 500ms));
    }
}

TEST_CASE("Profile destructor flushes pending modlist write", "[engine]") {
    auto dir = make_temp_dir("dtor_flush");
    {
        engine::profile::Profile profile(dir, 10s);  // long delay: nothing flushes
        profile.refresh_mod_status({"ModA"});
        profile.set_mod_enabled("ModA", false);
        REQUIRE_FALSE(fs::exists(dir / "modlist.txt"));
    }  // destructor flushes pending changes
    REQUIRE(fs::exists(dir / "modlist.txt"));
    REQUIRE(read_text(dir / "modlist.txt").find("-ModA") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Game-specific files
// ---------------------------------------------------------------------------

TEST_CASE("plugins/loadorder/lockedorder/archives round-trip", "[engine]") {
    auto dir = make_temp_dir("game_files");
    engine::profile::Profile profile(dir, 50ms);

    REQUIRE(profile.write_plugins({"Skyrim.esm", "Update.esm"}));
    REQUIRE(profile.read_plugins() == std::vector<std::string>({"Skyrim.esm", "Update.esm"}));

    REQUIRE(profile.write_load_order({"Update.esm", "Skyrim.esm"}));
    REQUIRE(profile.read_load_order() == std::vector<std::string>({"Update.esm", "Skyrim.esm"}));

    REQUIRE(profile.write_locked_order({{"Skyrim.esm", 0}, {"Update.esm", 1}}));
    const auto locked = profile.read_locked_order();
    REQUIRE(locked.size() == 2);
    REQUIRE(locked[0].name == "Skyrim.esm");
    REQUIRE(locked[0].priority == 0);
    REQUIRE(locked[1].name == "Update.esm");
    REQUIRE(locked[1].priority == 1);

    REQUIRE(profile.write_archives({"Skyrim - Textures.bsa"}));
    REQUIRE(profile.read_archives() == std::vector<std::string>({"Skyrim - Textures.bsa"}));
}

TEST_CASE("lockedorder skips malformed lines", "[engine]") {
    auto dir = make_temp_dir("locked_malformed");
    write_text(dir / "lockedorder.txt", "Good.esm|3\nBadNoPipe\nBadPrio.esm|notanumber\n");
    engine::profile::Profile profile(dir, 50ms);
    const auto locked = profile.read_locked_order();
    REQUIRE(locked.size() == 1);
    REQUIRE(locked[0].name == "Good.esm");
    REQUIRE(locked[0].priority == 3);
}

// ---------------------------------------------------------------------------
// Deletion
// ---------------------------------------------------------------------------

TEST_CASE("remove deletes the profile directory recursively", "[engine]") {
    auto dir = make_temp_dir("remove");
    write_text(dir / "settings.ini", "LocalSaves=true\n");
    write_text(dir / "modlist.txt", "+ModA\r\n");
    fs::create_directories(dir / "saves");
    write_text(dir / "saves" / "game.sav", "data");

    engine::profile::Profile profile(dir, 50ms);
    REQUIRE(profile.exists());
    REQUIRE(profile.remove() == engine::profile::ProfileRemoveResult::Removed);
    REQUIRE_FALSE(fs::exists(dir));
    REQUIRE_FALSE(profile.exists());
}

TEST_CASE("remove returns NotFound for a missing directory", "[engine]") {
    auto dir = make_temp_dir("remove_missing");
    fs::remove_all(dir);  // directory never created
    engine::profile::Profile profile(dir, 50ms);
    REQUIRE_FALSE(profile.exists());
    REQUIRE(profile.remove() == engine::profile::ProfileRemoveResult::NotFound);
}

TEST_CASE("remove refuses the active profile", "[engine]") {
    auto dir = make_temp_dir("remove_active");
    engine::profile::Profile profile(dir, 50ms);
    REQUIRE(profile.remove(/*is_active=*/true) ==
            engine::profile::ProfileRemoveResult::ActiveProfile);
    REQUIRE(fs::exists(dir));  // untouched
}

TEST_CASE("remove cancels a pending modlist write", "[engine]") {
    auto dir = make_temp_dir("remove_cancel");
    {
        engine::profile::Profile profile(dir, 10s);  // long debounce: nothing flushes
        profile.refresh_mod_status({"ModA"});
        profile.set_mod_enabled("ModA", false);  // schedules a delayed write
        REQUIRE(profile.remove() == engine::profile::ProfileRemoveResult::Removed);
    }  // destructor would flush pending writes — must be a no-op now
    REQUIRE_FALSE(fs::exists(dir));
}

TEST_CASE("remove reports partial failure on permission errors", "[engine]") {
    auto dir = make_temp_dir("remove_partial");
    fs::create_directories(dir / "locked");
    write_text(dir / "locked" / "file.txt", "x");
    fs::permissions(dir / "locked", fs::perms::owner_write | fs::perms::group_write,
                    fs::perm_options::remove);

    engine::profile::Profile profile(dir, 50ms);
    const auto result = profile.remove();
    if (geteuid() == 0) {
        // Root ignores permission bits — the removal succeeds.
        REQUIRE(result == engine::profile::ProfileRemoveResult::Removed);
    } else {
        REQUIRE(result == engine::profile::ProfileRemoveResult::PartialFailure);
        REQUIRE(fs::exists(dir));  // the locked subtree remains
    }
    fs::permissions(dir / "locked", fs::perms::owner_all, fs::perm_options::replace);
}