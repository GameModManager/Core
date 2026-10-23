// v3 P2 plaintext TOML game descriptors (Workspace-k8bc): the loader scans
// sorted .toml files, registers through the same RegistrationSink the
// compiled plugins use, and validates loudly (spec 3.6 + the DX/VAR
// revision's two-phase %VAR% rules). Qt-free engine tests; fake Platform
// adaptors stand in for the real ones.
//
// Uses the check() PASS/FAIL pattern (Release builds compile out assert()).

#include "engine/game/registry/game_knowledge.h"
#include "engine/game/registry/path_template.h"
#include "engine/game/registry/toml_descriptor.h"
#include "engine/plugin_host/plugin_loader.h"
#include "engine/source/game_catalog.h"
#include "engine/source/managed_games.h"
#include "engine/source/registry.h"
#include "test_platform.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using engine::PathTarget;
using engine::PluginLoader;
using gmm_test::InertPlatform;

namespace {

void check(bool cond, const std::string &msg) {
  INFO(msg);
  REQUIRE(cond);
}

// The spec's worked Valheim descriptor (DX/VAR revision 2.1), verbatim.
const char *kValheimToml = R"toml(
schema_version = 1

[game]
id             = "Valheim"
name           = "Valheim"
steam_appids   = [892970]
nexus_domain   = "valheim"
aliases        = []

binary      = { windows = "valheim.exe", linux = "valheim.x86_64", macos = "Valheim.app" }
executables = ["valheim.exe", "valheim.x86_64", "Valheim.app"]

mods_dir       = "%GAMEDIR%/BepInEx/plugins"
valid_dirs     = ["BepInEx", "doorstop_libs", "valheim_Data", "unstripped_corlib"]
valid_exts     = ["dll", "cfg", "json"]
case_sensitive = false
native_files   = ["valheim_Data/**", "doorstop_libs/**", "winhttp.dll",
                  "doorstop_config.ini", ".doorstop_version"]

[game.paths]
saves = { windows = "%LOCALLOW%/IronGate/Valheim", linux = "%APPDATA%/unity3d/IronGate/Valheim", macos = "%APPDATA%/IronGate/Valheim" }

[game.saves]
extensions = ["fch", "fwl", "db"]
subdir     = ""

[game.catalog]
download_sources = ["Nexus Mods"]
)toml";

// The spec's ghost game (OPTIONALITY revision B.1): only id/name/mods_dir are
// declared plus the saves data and the ui.tabs row that makes it reachable.
const char *kGhostToml = R"toml(
schema_version = 1

[game]
id       = "GhostGame"
name     = "Ghost Game"
mods_dir = "%GAMEDIR%/Mods"

[game.paths]
saves = "%DOCUMENTS%/GhostGame"

[game.saves]
extensions = ["sav"]

[[game.capabilities]]
id = "ui.tabs"

  [[game.capabilities.tabs]]
  capability = "saves"
  name       = "Saves"
  path       = "%DOCUMENTS%/GhostGame"
)toml";

void write_file(const fs::path &path, const std::string &content) {
  std::ofstream out(path, std::ios::binary);
  out << content;
}

fs::path make_temp(const std::string &tag) {
  static int counter = 0;
  const auto dir =
      fs::temp_directory_path() /
      ("gmm_tomldesc_" + tag + "_" + std::to_string(counter++));
  fs::create_directories(dir);
  return dir;
}

// Fake platforms mirroring path_template_test.cpp's four columns.
class FakeLinuxPlatform : public InertPlatform {
public:
  fs::path home         = "/home/u";
  fs::path documents    = "/home/u/Documents";
  fs::path appdata      = "/home/u/.config";
  fs::path localappdata = "/home/u/.local/share";
  fs::path steam        = "/steam";
  fs::path prefix       = "/steam/steamapps/compatdata/892970";
  fs::path prefix_user =
      "/steam/steamapps/compatdata/892970/pfx/drive_c/users/steamuser";

  [[nodiscard]] std::string platform_name() const override { return "linux"; }
  [[nodiscard]] fs::path home_dir() const override { return home; }
  [[nodiscard]] fs::path native_documents_dir() const override { return documents; }
  [[nodiscard]] fs::path native_appdata_dir() const override { return appdata; }
  [[nodiscard]] fs::path native_localappdata_dir() const override {
    return localappdata;
  }
  [[nodiscard]] fs::path find_steam_root() const override { return steam; }
  [[nodiscard]] fs::path steam_userdata_dir() const override {
    return steam / "userdata";
  }
  [[nodiscard]] fs::path resolve_proton_prefix(uint32_t) const override {
    return prefix;
  }
  [[nodiscard]] fs::path game_userprofile_dir(uint32_t) const override {
    return prefix_user;
  }
  [[nodiscard]] fs::path game_documents_dir(uint32_t) const override {
    return prefix_user / "Documents";
  }
  [[nodiscard]] fs::path game_local_appdata_dir(uint32_t) const override {
    return prefix_user / "AppData" / "Local";
  }
};

class FakeWindowsPlatform : public InertPlatform {
public:
  fs::path home = "C:/Users/u";

  [[nodiscard]] std::string platform_name() const override { return "windows"; }
  [[nodiscard]] fs::path home_dir() const override { return home; }
  [[nodiscard]] fs::path native_documents_dir() const override {
    return home / "Documents";
  }
  [[nodiscard]] fs::path native_appdata_dir() const override {
    return home / "AppData" / "Roaming";
  }
  [[nodiscard]] fs::path native_localappdata_dir() const override {
    return home / "AppData" / "Local";
  }
  [[nodiscard]] fs::path native_locallow_dir() const override {
    return home / "AppData" / "LocalLow";
  }
  [[nodiscard]] fs::path native_programdata_dir() const override {
    return "C:/ProgramData";
  }
};

class FakeMacPlatform : public InertPlatform {
public:
  fs::path home = "/Users/u";

  [[nodiscard]] std::string platform_name() const override { return "macos"; }
  [[nodiscard]] fs::path home_dir() const override { return home; }
  [[nodiscard]] fs::path native_documents_dir() const override {
    return home / "Documents";
  }
  [[nodiscard]] fs::path native_appdata_dir() const override {
    return home / "Library" / "Application Support";
  }
  [[nodiscard]] fs::path native_localappdata_dir() const override {
    return home / "Library" / "Application Support";
  }
  [[nodiscard]] fs::path native_programdata_dir() const override {
    return "/Library/Application Support";
  }
};

std::string expand_str(const std::string &tmpl, const InertPlatform &platform,
                       PathTarget target, uint32_t appid = 0,
                       const fs::path &game_dir = "/games/valheim") {
  std::string error;
  const auto path =
      engine::expand_path_template(tmpl, &platform, target, appid, game_dir, error);
  if (!error.empty())
    return "<error:" + error + ">";
  return path.string();
}

}  // namespace

TEST_CASE("valheim descriptor resolves saves paths on all four targets",
          "[engine]") {
  const auto dir = make_temp("valheim");
  write_file(dir / "Valheim.toml", kValheimToml);

  PluginLoader loader;
  check(engine::load_toml_game_dir(dir, loader) == 1,
        "one record registered from one file");

  // Identity went through the same sink as the compiled plugins.
  const auto rows = loader.game_plugins();
  check(rows.size() == 1, "one game_plugins row");
  check(rows[0].game_id == "Valheim", "game_id");
  check(rows[0].game_display_name == "Valheim", "display name");
  check(rows[0].steam_appid == 892970, "steam appid feeds detection");
  check(rows[0].nexus_domain == "valheim", "nexus domain feeds NXM routing");
  check(rows[0].exe_windows == "valheim.exe", "binary windows cell");
  check(rows[0].exe_linux == "valheim.x86_64", "binary linux cell");
  check(rows[0].exe_macos == "Valheim.app", "binary macos cell");

  // Data capabilities landed as the same knowledge keys the v2 hooks write.
  check(loader.knowledge().get("Valheim", "mods_subpath", "") ==
            "BepInEx/plugins",
        "mods_dir strips its %GAMEDIR%/ prefix into the deploy subpath");
  check(loader.knowledge().get("Valheim", "executables", "") ==
            "valheim.exe,valheim.x86_64,Valheim.app",
        "executables feed the launch list and the .exe target rule");
  check(loader.knowledge().get("Valheim", "mod_valid_dirs", "") ==
            "BepInEx,doorstop_libs,valheim_Data,unstripped_corlib",
        "valid_dirs");
  check(loader.knowledge().get("Valheim", "mod_valid_exts", "") == "dll,cfg,json",
        "valid_exts");
  check(loader.knowledge().get("Valheim", "case_sensitive", "") == "false",
        "case_sensitive override");
  check(loader.knowledge().get("Valheim", "save_extensions", "") == "fch,fwl,db",
        "save extensions");
  check(loader.knowledge().get("Valheim", "download_sources", "") == "Nexus Mods",
        "download sources");

  // paths.saves is stored as raw templates (new state; the expander is the
  // resolution path until a consumer wires it - spec G4).
  const auto &knowledge = loader.knowledge();
  check(knowledge.get("Valheim", "saves_root_windows", "") ==
            "%LOCALLOW%/IronGate/Valheim",
        "windows cell stored raw");
  check(knowledge.get("Valheim", "saves_root_linux", "") ==
            "%APPDATA%/unity3d/IronGate/Valheim",
        "linux cell stored raw");
  check(knowledge.get("Valheim", "saves_root_macos", "") ==
            "%APPDATA%/IronGate/Valheim",
        "macos cell stored raw");

  // The four columns of the token table, resolved through fake platforms.
  FakeWindowsPlatform windows;
  FakeLinuxPlatform linux_host;
  FakeMacPlatform mac;

  check(expand_str(knowledge.get("Valheim", "saves_root_windows", ""), windows,
                   PathTarget::Windows) == "C:/Users/u/AppData/LocalLow/IronGate/Valheim",
        "windows native");
  check(expand_str(knowledge.get("Valheim", "saves_root_linux", ""), linux_host,
                   PathTarget::Linux) == "/home/u/.config/unity3d/IronGate/Valheim",
        "linux native");
  check(expand_str(knowledge.get("Valheim", "saves_root_macos", ""), mac,
                   PathTarget::MacOS) ==
            "/Users/u/Library/Application Support/IronGate/Valheim",
        "macos native");
  check(expand_str(knowledge.get("Valheim", "saves_root_windows", ""), linux_host,
                   PathTarget::Windows, 892970) ==
            "/steam/steamapps/compatdata/892970/pfx/drive_c/users/steamuser/"
            "AppData/LocalLow/IronGate/Valheim",
        "linux host, windows exe under Proton");
}

TEST_CASE("ghost game loads with every undeclared surface absent", "[engine]") {
  const auto dir = make_temp("ghost");
  write_file(dir / "GhostGame.toml", kGhostToml);

  PluginLoader loader;
  check(engine::load_toml_game_dir(dir, loader) == 1, "ghost registers");

  const auto rows = loader.game_plugins();
  check(rows.size() == 1, "one row");
  check(rows[0].game_id == "GhostGame", "id");
  check(rows[0].game_display_name == "Ghost Game", "name");
  check(rows[0].steam_appid == 0, "no store id: never Steam-detected");
  check(rows[0].nexus_domain.empty(),
        "no nexus domain: no NXM routing, no Visit-on-Nexus");
  check(rows[0].gog_id.empty() && rows[0].epic_namespace.empty(),
        "no store provenance");

  // Absence of wiring, stated as absence: nothing may fabricate a default.
  check(!loader.knowledge().has("GhostGame", "download_sources"),
        "no download-source wiring");
  check(!loader.knowledge().has("GhostGame", "game_icon_url"),
        "no icon fetch");
  check(!loader.knowledge().has("GhostGame", "masterlist_url"),
        "no masterlist prefetch");
  check(!loader.knowledge().has("GhostGame", "executables"),
        "no launch seed");
  check(!loader.knowledge().has("GhostGame", "mod_valid_dirs"),
        "every dir under Mods/ is scanned");
  check(!loader.knowledge().has("GhostGame", "case_sensitive"),
        "case_sensitive default (true) applies by absence");

  // The declared surfaces did land.
  check(loader.knowledge().get("GhostGame", "mods_subpath", "") == "Mods",
        "mods_dir");
  check(loader.knowledge().get("GhostGame", "saves_root", "") ==
            "%DOCUMENTS%/GhostGame",
        "plain saves template stored raw");
  check(loader.knowledge().get("GhostGame", "save_extensions", "") == "sav",
        "declared save extensions");

  // --list-games shows it with file provenance and no derived sources.
  engine::ManagedGames managed(fs::path("/nonexistent/managed_games.json"));
  const auto doc    = engine::build_supported_games(loader, managed,
                                                    engine::Source::Registry::instance());
  const auto &games = doc["games"];
  check(games.size() == 1, "one catalog entry");
  check(games[0]["provenance"] == "file", "provenance is file, not plugin");
  check(games[0]["plugin"] == "GhostGame.toml", "the descriptor file is named");
  check(games[0]["sources"].empty(),
        "no source rows: nothing is derived for a domainless, idless game");
}

TEST_CASE("missing required field refuses that record, others still load",
          "[engine]") {
  const auto dir = make_temp("required");
  write_file(dir / "A_Bad.toml",
             "schema_version = 1\n[game]\nid = \"Bad\"\nmods_dir = \"Mods\"\n");
  write_file(dir / "B_Good.toml",
             "schema_version = 1\n[game]\nid = \"Good\"\nname = \"Good\"\n"
             "mods_dir = \"Mods\"\n");

  PluginLoader loader;
  check(engine::load_toml_game_dir(dir, loader) == 1,
        "only the valid record registers");
  check(loader.knowledge().registered_games().size() == 1,
        "exactly one game in the registry");
  check(loader.knowledge().registered_games()[0] == "Good", "the good one");
  // No half-game: the refused record left nothing behind.
  check(!loader.knowledge().has("Bad", "mods_subpath"),
        "the refused record registered no knowledge");
  check(loader.game_plugins().size() == 1, "no instance-backing row for Bad");
}

TEST_CASE("unknown key warns and the load continues", "[engine]") {
  const auto dir = make_temp("unknown");
  write_file(dir / "Game.toml",
             "schema_version = 1\n[game]\nid = \"G\"\nname = \"G\"\n"
             "mods_dir = \"Mods\"\nbogus_future_key = \"x\"\n");

  PluginLoader loader;
  check(engine::load_toml_game_dir(dir, loader) == 1,
        "unknown key is a warning, never a failure");
  check(loader.game_plugins().size() == 1, "the record still registered");
}

TEST_CASE("selected-cell-only expansion", "[engine]") {
  const auto dir = make_temp("cells");
  // %LOCALLOW% is a KNOWN token, so phase A accepts both cells even though it
  // is undefined on linux - that is what makes Valheim's table legal.
  write_file(dir / "Game.toml",
             "schema_version = 1\n[game]\nid = \"G\"\nname = \"G\"\n"
             "mods_dir = \"Mods\"\n"
             "[game.paths]\n"
             "saves = { windows = \"%LOCALLOW%/w\", linux = \"%LOCALLOW%/l\" }\n");

  PluginLoader loader;
  check(engine::load_toml_game_dir(dir, loader) == 1,
        "known-but-undefined tokens in unselected cells load fine");

  FakeLinuxPlatform platform;
  // The SELECTED cell (windows, under Proton) expands; the unselected linux
  // cell would error, but only if it were the selected one.
  check(expand_str(loader.knowledge().get("G", "saves_root_windows", ""),
                   platform, PathTarget::Windows, 892970)
                .find("AppData/LocalLow/w") != std::string::npos,
        "the selected windows cell resolves under Proton");
  check(expand_str(loader.knowledge().get("G", "saves_root_linux", ""), platform,
                   PathTarget::Linux)
                .find("<error:") == 0,
        "the linux cell errors only when IT is the selected target");
}

TEST_CASE("unknown token in a cell is a phase-A load error", "[engine]") {
  const auto dir = make_temp("phasea");
  write_file(dir / "Bad.toml",
             "schema_version = 1\n[game]\nid = \"Bad\"\nname = \"Bad\"\n"
             "mods_dir = \"Mods\"\n"
             "[game.paths]\n"
             "saves = \"%BOGUS%/x\"\n");
  write_file(dir / "Good.toml",
             "schema_version = 1\n[game]\nid = \"Good\"\nname = \"Good\"\n"
             "mods_dir = \"Mods\"\n");

  PluginLoader loader;
  check(engine::load_toml_game_dir(dir, loader) == 1,
        "the bad record is refused, the good one still loads");
  check(!loader.knowledge().has("Bad", "saves_root"),
        "nothing from the refused record registers");
  check(loader.knowledge().registered_games().size() == 1, "only Good");
}

TEST_CASE("parse error skips that file, others still load", "[engine]") {
  const auto dir = make_temp("parse");
  write_file(dir / "Broken.toml", "schema_version = 1\n[game\nid = \"x\"\n");
  write_file(dir / "Fine.toml",
             "schema_version = 1\n[game]\nid = \"Fine\"\nname = \"Fine\"\n"
             "mods_dir = \"Mods\"\n");

  PluginLoader loader;
  check(engine::load_toml_game_dir(dir, loader) == 1,
        "the broken file is skipped, the fine one loads");
  check(loader.knowledge().registered_games().size() == 1, "only Fine");
}

TEST_CASE("array form registers every record", "[engine]") {
  const auto dir = make_temp("array");
  write_file(dir / "Pack.toml",
             "schema_version = 1\n"
             "[[games]]\nid = \"One\"\nname = \"One\"\nmods_dir = \"M\"\n"
             "[[games]]\nid = \"Two\"\nname = \"Two\"\nmods_dir = \"M\"\n");

  PluginLoader loader;
  check(engine::load_toml_game_dir(dir, loader) == 2, "both records register");
  check(loader.game_plugins().size() == 2, "two rows");
}

TEST_CASE("duplicate game_id across files keeps the first by filename",
          "[engine]") {
  const auto dir = make_temp("dup");
  write_file(dir / "A_First.toml",
             "schema_version = 1\n[game]\nid = \"Dup\"\nname = \"First\"\n"
             "mods_dir = \"M\"\n");
  write_file(dir / "B_Second.toml",
             "schema_version = 1\n[game]\nid = \"Dup\"\nname = \"Second\"\n"
             "mods_dir = \"M\"\n");

  PluginLoader loader;
  check(engine::load_toml_game_dir(dir, loader) == 2,
        "both records register (the loser keeps its record)");
  check(loader.game_plugins().size() == 1,
        "only the first by filename can back an instance");
  check(loader.game_plugins()[0].game_display_name == "First", "the first wins");
}

TEST_CASE("a lone percent stays literal through the loader", "[engine]") {
  const auto dir = make_temp("percent");
  write_file(dir / "Game.toml",
             "schema_version = 1\n[game]\nid = \"G\"\nname = \"G\"\n"
             "mods_dir = \"Mods\"\n"
             "[game.paths]\n"
             "saves = \"100%/saves\"\n");

  PluginLoader loader;
  check(engine::load_toml_game_dir(dir, loader) == 1,
        "100% is not a malformed token");
  check(loader.knowledge().get("G", "saves_root", "") == "100%/saves",
        "the raw template keeps the lone %");
  check(expand_str(loader.knowledge().get("G", "saves_root", ""),
                   FakeLinuxPlatform{}, PathTarget::Linux) == "100%/saves",
        "expansion leaves it literal");
}
