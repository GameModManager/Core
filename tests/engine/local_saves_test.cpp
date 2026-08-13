// Per-profile local saves (MO2 GamebryoLocalSavegames parity): the INI rewrite
// (sLocalSavePath=__MO_Saves\, bUseMyGamesDirectory=1), the savepath.ini
// backup/restore, the profile saves dir, and the bind-mount pair the launch
// layer installs. Uses temp dirs only - no VFS, no overlay.
#include "engine/saves/local_saves.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <utility>
#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;

namespace {
void require(bool cond, const std::string& msg) {
    INFO(msg);
    REQUIRE(cond);
}
}

static std::string read_file(const fs::path& p) {
    std::ifstream in(p);
    std::string out, line;
    while (std::getline(in, line)) out += line + "\n";
    return out;
}

static fs::path make_mygames() {
    static int counter = 0;
    auto dir = fs::temp_directory_path() / ("gmm_local_saves_" + std::to_string(getpid()) + "_" + std::to_string(counter++));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

TEST_CASE("local saves", "[engine]") {
    const auto mygames = make_mygames();
    const auto instance = mygames / "instance";
    fs::create_directories(instance);

    // A stock Skyrim SE ini: comments + unrelated keys + a PRE-EXISTING
    // local-save location (so enabling backs it up).
    const fs::path ini = mygames / "Skyrimcustom.ini";
    {
        std::ofstream out(ini);
        out << "; Skyrim custom config\n"
            << "[General]\n"
            << "sTest=value1\n"
            << "sLocalSavePath=Saves/Quicksave/\n"
            << "[Display]\n"
            << "sCustom=keepme\n";
    }

    auto cfg = engine::resolve_local_saves(mygames, instance, "Default",
                                           "Skyrimcustom.ini", true);
    require(cfg.enabled, "resolve: enabled");
    require(cfg.ini_path == ini, "resolve: ini path");
    require(cfg.profile_saves_dir == instance / "profiles" / "Default" / "saves",
            "resolve: profile saves dir");
    require(cfg.backup_path == instance / "profiles" / "Default" / "savepath.ini",
            "resolve: backup path");
    require(cfg.local_saves_dir == mygames / "__MO_Saves",
            "resolve: local saves dir");

    // Enable: rewrites the INI, creates both dirs, backs up prior value.
    bool changed = engine::apply_local_saves(cfg);
    require(changed, "apply(enabled): changed");
    auto contents = read_file(ini);
    require(contents.find("sLocalSavePath=__MO_Saves\\") != std::string::npos,
            "enable: sLocalSavePath written");
    require(contents.find("bUseMyGamesDirectory=1") != std::string::npos,
            "enable: bUseMyGamesDirectory written");
    require(contents.find("sTest=value1") != std::string::npos,
            "enable: unrelated General key preserved");
    require(contents.find("sCustom=keepme") != std::string::npos,
            "enable: unrelated section key preserved");
    require(contents.find("; Skyrim custom config") != std::string::npos,
            "enable: comment preserved");
    require(fs::is_directory(cfg.profile_saves_dir), "enable: profile dir created");
    require(fs::is_directory(cfg.local_saves_dir), "enable: local dir created");

    auto mount = engine::local_saves_mount(cfg);
    require(mount.first == cfg.profile_saves_dir, "mount: source is profile dir");
    require(mount.second == cfg.local_saves_dir, "mount: target is local dir");

    // Idempotent: second enable is a no-op (no double backup).
    changed = engine::apply_local_saves(cfg);
    require(!changed, "apply(enabled): second call unchanged");
    auto backup = read_file(cfg.backup_path);
    require(backup.find("sLocalSavePath=") != std::string::npos,
            "enable: backup holds prior value");

    // Disable: restores the backed-up value and removes the backup.
    auto disabled = engine::resolve_local_saves(mygames, instance, "Default",
                                                "Skyrimcustom.ini", false);
    changed = engine::apply_local_saves(disabled);
    require(changed, "apply(disabled): changed");
    contents = read_file(ini);
    require(contents.find("sTest=value1") != std::string::npos,
            "disable: unrelated key still present");
    require(contents.find("sLocalSavePath=") == std::string::npos ||
            contents.find("sLocalSavePath=__MO_Saves\\") == std::string::npos,
            "disable: local-save key gone or restored to non-local");
    require(!fs::exists(cfg.backup_path), "disable: backup removed");
    auto mount_off = engine::local_saves_mount(disabled);
    require(mount_off.first.empty(), "mount: empty when disabled");

    // Fresh ini with NO prior keys: enabling backs up nothing, disabling
    // deletes the keys outright (no savepath.ini left behind).
    const fs::path ini2 = mygames / "Skyrimcustom2.ini";
    {
        std::ofstream out(ini2);
        out << "[General]\n"
            << "sOther=x\n";
    }
    auto cfg2 = engine::resolve_local_saves(mygames, instance, "Default",
                                            "Skyrimcustom2.ini", true);
    changed = engine::apply_local_saves(cfg2);
    require(changed, "no-backup enable: changed");
    require(!fs::exists(cfg2.backup_path), "no-backup enable: no backup written");
    auto dis2 = engine::resolve_local_saves(mygames, instance, "Default",
                                            "Skyrimcustom2.ini", false);
    changed = engine::apply_local_saves(dis2);
    require(changed, "no-backup disable: changed");
    contents = read_file(ini2);
    require(contents.find("sOther=x") != std::string::npos,
            "no-backup disable: unrelated key preserved");
    require(contents.find("sLocalSavePath") == std::string::npos,
            "no-backup disable: local-save key removed");
    require(contents.find("bUseMyGamesDirectory") == std::string::npos,
            "no-backup disable: mygames key removed");

    // Disabled resolve with missing inputs stays off.
    auto bad = engine::resolve_local_saves({}, instance, "Default", "x.ini", true);
    require(!bad.enabled, "resolve: empty mygames -> disabled");

    fs::remove_all(mygames);
}
