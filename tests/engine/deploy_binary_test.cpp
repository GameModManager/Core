// Engine regression test for the executable-copy deploy contract.
//
// Executables and scripts resolve sibling files relative to their OWN
// location. A symlinked file in the overlay staging lowerdir resolves through
// to the mod folder when run (/proc/self/exe, Wine path canonicalization), so
// skse64_loader.exe would look for SkyrimSE.exe inside the mod folder and
// fail. The deploy must therefore place binaries as REAL files in staging.
//
// Detection is deliberate (is_executable_binary): .exe / .elf / .sh, plus
// extensionless files that are ELF binaries or #! scripts, are copied.
// Everything else - meshes, textures, .bin blobs, plugins - is only ever read
// from within the merged view, never run, so it stays a symlink.
#include "engine/deploy/deploy_utils.h"
#include "engine/deploy/strategy.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;

namespace {
void check(bool cond, const char* what) {
    INFO(what);
    REQUIRE(cond);
}
}

static void write_bytes(const fs::path& p, const std::string& contents) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!out.good()) {
        std::printf("FAIL: could not write %s\n", p.string().c_str());
        std::exit(1);
    }
}

static std::string read_bytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

static bool is_exec(const fs::path& p) {
    std::error_code ec;
    auto perms = fs::status(p, ec).permissions();
    return !ec && (perms & fs::perms::owner_exec) != fs::perms::none;
}

// The ELF magic is written as two string literals on purpose: a bare
// "\x7fELF" would slurp 'E' into the hex escape (\x7fE).
static const std::string kElf = "\x7f" "ELF\x02\x01\x01\x00";
static const std::string kShebang = "#!/bin/sh\necho hi\n";
static const std::string kPe = "MZ\x90\x00\x03\x00";
static const std::string kText = "just data, not executable\n";

static void make_instance(const fs::path& root) {
    const fs::path mod = root / "mods" / "ExecMod";
    write_bytes(mod / "launcher.sh", kShebang);
    write_bytes(mod / "game.exe", kPe);
    write_bytes(mod / "game.elf", kElf);
    write_bytes(mod / "binless", kElf);       // ELF, no extension
    write_bytes(mod / "scriptless", kShebang);  // #!, no extension
    write_bytes(mod / "notes.txt", kText);
    write_bytes(mod / "tex.dds", kText);
    write_bytes(mod / "blob.bin", kText);     // data blob, NOT an exe
}

TEST_CASE("deploy binary", "[engine]") {
    // --- is_executable_binary unit checks ---
    const fs::path tmp =
        fs::current_path() / ("gmm_deploy_binary_" + std::to_string(getpid()));
    fs::create_directories(tmp);

    write_bytes(tmp / "a.exe", kPe);
    write_bytes(tmp / "B.ELF", kElf);
    write_bytes(tmp / "c.sh", kShebang);
    write_bytes(tmp / "d", kElf);
    write_bytes(tmp / "e", kShebang);
    write_bytes(tmp / "f", kText);         // extensionless but plain data
    write_bytes(tmp / "g.bin", kText);
    write_bytes(tmp / "h.dds", kText);
    write_bytes(tmp / "i.exe.notes", kText);  // look-alike extension

    check(engine::is_executable_binary(tmp / "a.exe"), ".exe is a binary");
    check(engine::is_executable_binary(tmp / "B.ELF"), ".ELF is a binary (case-insensitive)");
    check(engine::is_executable_binary(tmp / "c.sh"), ".sh is a binary");
    check(engine::is_executable_binary(tmp / "d"), "extensionless ELF is a binary");
    check(engine::is_executable_binary(tmp / "e"), "extensionless shebang script is a binary");
    check(!engine::is_executable_binary(tmp / "f"), "extensionless plain data is not a binary");
    check(!engine::is_executable_binary(tmp / "g.bin"), ".bin data blob is not a binary");
    check(!engine::is_executable_binary(tmp / "h.dds"), ".dds is not a binary");
    check(!engine::is_executable_binary(tmp / "i.exe.notes"), "fake .exe extension look-alike is not a binary");

    // --- OverlayFS staging deploy: binaries become real files ---
    const fs::path base = tmp / "instance";
    make_instance(base);

    const fs::path staging = base / ".gmm_staging";
    const bool ok = engine::deploy_all_enabled_mods(
        base / "mods", staging, "Data", /*deploy_include_mod_id=*/false, "");
    check(ok, "deploy_all_enabled_mods succeeds");

    const auto data = staging / "Data";
    const fs::path sh = data / "launcher.sh";
    check(fs::is_regular_file(sh) && !fs::is_symlink(sh),
          "deployed .sh is a real file, not a symlink");
    check(read_bytes(sh) == kShebang, "deployed .sh content matches the source");
    check(is_exec(sh), "deployed .sh carries the exec bit");

    const fs::path exe = data / "game.exe";
    check(fs::is_regular_file(exe) && !fs::is_symlink(exe),
          "deployed .exe is a real file, not a symlink");
    check(read_bytes(exe) == kPe, "deployed .exe content matches the source");
    check(is_exec(exe), "deployed .exe carries the exec bit");

    const fs::path elf = data / "game.elf";
    check(fs::is_regular_file(elf) && !fs::is_symlink(elf),
          "deployed .elf is a real file, not a symlink");

    const fs::path binless = data / "binless";
    check(fs::is_regular_file(binless) && !fs::is_symlink(binless),
          "deployed extensionless ELF is a real file, not a symlink");

    const fs::path scriptless = data / "scriptless";
    check(fs::is_regular_file(scriptless) && !fs::is_symlink(scriptless),
          "deployed extensionless shebang script is a real file, not a symlink");

    // Data files stay symlinks - they are read, never run.
    check(fs::is_symlink(data / "notes.txt"), "deployed .txt stays a symlink");
    check(fs::is_symlink(data / "tex.dds"), "deployed .dds stays a symlink");
    check(fs::is_symlink(data / "blob.bin"), "deployed .bin data stays a symlink");

    // Redeploy into the same staging dir must not fail (copy over the prior
    // copy) and the binaries must still be real files.
    const bool ok2 = engine::deploy_all_enabled_mods(
        base / "mods", staging, "Data", /*deploy_include_mod_id=*/false, "");
    check(ok2, "redeploy over existing staging succeeds");
    check(fs::is_regular_file(sh) && !fs::is_symlink(sh),
          "redeployed .sh is still a real file");
    check(read_bytes(sh) == kShebang, "redeployed .sh content still matches");

    // --- SymlinkStrategy fallback: same contract directly into game_dir ---
    const fs::path game = tmp / "game";
    const fs::path execmod = base / "mods" / "ExecMod";
    engine::SymlinkStrategy direct(/*case_sensitive=*/true);
    check(direct.deploy(execmod / "launcher.sh", game / "launcher.sh"),
          "SymlinkStrategy deploys .sh");
    check(direct.deploy(execmod / "notes.txt", game / "notes.txt"),
          "SymlinkStrategy deploys .txt");
    check(fs::is_regular_file(game / "launcher.sh") && !fs::is_symlink(game / "launcher.sh"),
          "SymlinkStrategy .sh is a real file");
    check(fs::is_symlink(game / "notes.txt"),
          "SymlinkStrategy .txt stays a symlink");

    fs::remove_all(tmp);
}
