// Engine test for profile creation (engine/profile/profile_creation) — fresh
// profile creation with defaults + game-plugin initialization callback, copy
// profile with preserved mod/plugin state, name validation, and profile
// listing. Uses temp dirs only, no Qt.
#include "engine/profile/profile.h"
#include "engine/profile/profile_creation.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::atomic<int> g_counter{0};

fs::path make_temp_dir(const char *tag) {
  auto dir = fs::temp_directory_path() /
             ("gmm_profile_creation_" + std::string(tag) + "_" +
              std::to_string(getpid()) + "_" + std::to_string(g_counter.fetch_add(1)));
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

std::string read_text(const fs::path &p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void write_text(const fs::path &p, const std::string &content) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << content;
}

}  // namespace

// ---------------------------------------------------------------------------
// Fresh profile
// ---------------------------------------------------------------------------

TEST_CASE("fresh profile creates directory with default settings", "[engine]") {
  auto root               = make_temp_dir("fresh");
  const auto profiles_dir = root / "profiles";

  auto result = engine::profile::create_fresh_profile(profiles_dir, "Default");
  REQUIRE(result.success);
  REQUIRE(result.error.empty());
  REQUIRE(result.directory == profiles_dir / "Default");
  REQUIRE(fs::is_directory(result.directory));

  // settings.ini initialized with defaults.
  const std::string settings = read_text(result.directory / "settings.ini");
  REQUIRE(settings.find("LocalSaves=false") != std::string::npos);
  REQUIRE(settings.find("LocalSettings=false") != std::string::npos);
  REQUIRE(settings.find("AutomaticArchiveInvalidation=false") != std::string::npos);

  // modlist.txt and archives.txt created empty.
  REQUIRE(read_text(result.directory / "modlist.txt").empty());
  REQUIRE(read_text(result.directory / "archives.txt").empty());

  // The profile is visible to the profile list.
  const auto profiles = engine::profile::list_profiles(profiles_dir);
  REQUIRE(profiles == std::vector<std::string>({"Default"}));
}

TEST_CASE("fresh profile loads into Profile with defaults", "[engine]") {
  auto root               = make_temp_dir("fresh_load");
  const auto profiles_dir = root / "profiles";

  auto result = engine::profile::create_fresh_profile(profiles_dir, "Test");
  REQUIRE(result.success);

  engine::profile::ProfileManager profile(result.directory);
  REQUIRE_FALSE(profile.local_saves());
  REQUIRE_FALSE(profile.local_settings());
  REQUIRE_FALSE(profile.automatic_archive_invalidation());
  REQUIRE(profile.mods().empty());
}

TEST_CASE("fresh profile calls game init and auto-detects local settings", "[engine]") {
  auto root               = make_temp_dir("game_init");
  const auto profiles_dir = root / "profiles";

  bool init_called = false;
  auto result      = engine::profile::create_fresh_profile(
      profiles_dir, "WithGame", [&](const fs::path &dir) {
        init_called = true;
        // Game plugin copies a game INI and creates a saves dir.
        write_text(dir / "Skyrim.ini", "[General]\nsLanguage=ENGLISH\n");
        fs::create_directories(dir / "saves");
      });
  REQUIRE(result.success);
  REQUIRE(init_called);

  engine::profile::ProfileManager profile(result.directory);
  REQUIRE(profile.local_saves());     // saves/ exists
  REQUIRE(profile.local_settings());  // game INI present
  REQUIRE_FALSE(profile.automatic_archive_invalidation());
}

TEST_CASE("fresh profile detects _saves as local saves off", "[engine]") {
  auto root               = make_temp_dir("underscore_saves");
  const auto profiles_dir = root / "profiles";

  auto result = engine::profile::create_fresh_profile(
      profiles_dir, "NoLocalSaves", [](const fs::path &dir) {
        fs::create_directories(dir / "_saves");
      });
  REQUIRE(result.success);

  engine::profile::ProfileManager profile(result.directory);
  REQUIRE_FALSE(profile.local_saves());
}

TEST_CASE("fresh profile without game init keeps defaults", "[engine]") {
  auto root               = make_temp_dir("no_game_init");
  const auto profiles_dir = root / "profiles";

  auto result = engine::profile::create_fresh_profile(profiles_dir, "Plain");
  REQUIRE(result.success);

  engine::profile::ProfileManager profile(result.directory);
  REQUIRE_FALSE(profile.local_saves());
  REQUIRE_FALSE(profile.local_settings());
}

// ---------------------------------------------------------------------------
// Name validation / edge cases
// ---------------------------------------------------------------------------

TEST_CASE("invalid profile names are rejected", "[engine]") {
  auto root               = make_temp_dir("invalid");
  const auto profiles_dir = root / "profiles";

  for (const char *bad : {"", ".", "..", "a/b", "a\\b"}) {
    std::string error;
    REQUIRE_FALSE(engine::profile::is_valid_profile_name(bad, &error));
    REQUIRE_FALSE(error.empty());

    auto result = engine::profile::create_fresh_profile(profiles_dir, bad);
    REQUIRE_FALSE(result.success);
    REQUIRE_FALSE(result.error.empty());
    REQUIRE(result.directory.empty());
  }
  // Nothing was created for any invalid name.
  REQUIRE(engine::profile::list_profiles(profiles_dir).empty());
}

TEST_CASE("creating an existing profile fails", "[engine]") {
  auto root               = make_temp_dir("exists");
  const auto profiles_dir = root / "profiles";

  auto first = engine::profile::create_fresh_profile(profiles_dir, "Default");
  REQUIRE(first.success);

  auto second = engine::profile::create_fresh_profile(profiles_dir, "Default");
  REQUIRE_FALSE(second.success);
  REQUIRE(second.error.find("already exists") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Copy profile
// ---------------------------------------------------------------------------

TEST_CASE("copy profile preserves mod/plugin state and updates name", "[engine]") {
  auto root               = make_temp_dir("copy");
  const auto profiles_dir = root / "profiles";

  // Source profile with real state.
  auto source = engine::profile::create_fresh_profile(profiles_dir, "Source");
  REQUIRE(source.success);
  write_text(source.directory / "modlist.txt",
             "# This file was automatically generated by GameModManager.\r\n"
             "+ModA\r\n"
             "-ModB\r\n"
             "*DLC1\r\n");
  write_text(source.directory / "plugins.txt", "Skyrim.esm\nUpdate.esm\n");
  write_text(source.directory / "loadorder.txt", "Update.esm\nSkyrim.esm\n");
  write_text(source.directory / "archives.txt", "Skyrim - Textures.bsa\n");
  write_text(source.directory / "Skyrim.ini", "[General]\nsLanguage=ENGLISH\n");
  fs::create_directories(source.directory / "saves");
  write_text(source.directory / "saves" / "save1.ess", "data");

  auto result = engine::profile::copy_profile(profiles_dir, "Copy", source.directory);
  REQUIRE(result.success);
  REQUIRE(result.directory == profiles_dir / "Copy");
  REQUIRE(fs::is_directory(result.directory));

  // All mod/plugin state preserved verbatim.
  REQUIRE(read_text(result.directory / "modlist.txt") ==
          read_text(source.directory / "modlist.txt"));
  REQUIRE(read_text(result.directory / "plugins.txt") ==
          read_text(source.directory / "plugins.txt"));
  REQUIRE(read_text(result.directory / "loadorder.txt") ==
          read_text(source.directory / "loadorder.txt"));
  REQUIRE(read_text(result.directory / "archives.txt") ==
          read_text(source.directory / "archives.txt"));
  REQUIRE(read_text(result.directory / "Skyrim.ini") ==
          read_text(source.directory / "Skyrim.ini"));
  REQUIRE(read_text(result.directory / "saves" / "save1.ess") == "data");

  // settings.ini records the new profile name; other settings preserved.
  const std::string settings = read_text(result.directory / "settings.ini");
  REQUIRE(settings.find("ProfileName=Copy") != std::string::npos);
  REQUIRE(settings.find("LocalSaves=false") != std::string::npos);

  // Both profiles are listed.
  const auto profiles = engine::profile::list_profiles(profiles_dir);
  REQUIRE(profiles == std::vector<std::string>({"Copy", "Source"}));
}

TEST_CASE("copy profile preserves local saves/settings flags", "[engine]") {
  auto root               = make_temp_dir("copy_flags");
  const auto profiles_dir = root / "profiles";

  auto source = engine::profile::create_fresh_profile(
      profiles_dir, "Source", [](const fs::path &dir) {
        write_text(dir / "Skyrim.ini", "[General]\n");
        fs::create_directories(dir / "saves");
      });
  REQUIRE(source.success);

  auto result = engine::profile::copy_profile(profiles_dir, "Copy", source.directory);
  REQUIRE(result.success);

  engine::profile::ProfileManager copied(result.directory);
  REQUIRE(copied.local_saves());
  REQUIRE(copied.local_settings());
  REQUIRE(copied.name() == "Copy");
}

TEST_CASE("copy profile rejects invalid names and missing sources", "[engine]") {
  auto root               = make_temp_dir("copy_errors");
  const auto profiles_dir = root / "profiles";

  auto source = engine::profile::create_fresh_profile(profiles_dir, "Source");
  REQUIRE(source.success);

  auto bad_name = engine::profile::copy_profile(profiles_dir, "a/b", source.directory);
  REQUIRE_FALSE(bad_name.success);

  auto missing =
      engine::profile::copy_profile(profiles_dir, "Copy", root / "does_not_exist");
  REQUIRE_FALSE(missing.success);
  REQUIRE(missing.error.find("does not exist") != std::string::npos);

  auto dup = engine::profile::copy_profile(profiles_dir, "Source", source.directory);
  REQUIRE_FALSE(dup.success);
  REQUIRE(dup.error.find("already exists") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Listing
// ---------------------------------------------------------------------------

TEST_CASE("list_profiles returns sorted names and handles missing dir", "[engine]") {
  auto root               = make_temp_dir("list");
  const auto profiles_dir = root / "profiles";

  REQUIRE(engine::profile::list_profiles(profiles_dir).empty());

  REQUIRE(engine::profile::create_fresh_profile(profiles_dir, "Zeta").success);
  REQUIRE(engine::profile::create_fresh_profile(profiles_dir, "Alpha").success);
  REQUIRE(engine::profile::create_fresh_profile(profiles_dir, "Mid").success);

  const auto profiles = engine::profile::list_profiles(profiles_dir);
  REQUIRE(profiles == std::vector<std::string>({"Alpha", "Mid", "Zeta"}));
}

// ---------------------------------------------------------------------------
// Rename a mod in every profile (MO2 Profile::renameModInAllProfiles)
// ---------------------------------------------------------------------------

// A mod's identity is its folder name, and each profile records that name
// with the mod's priority (line order) and enabled state. Renaming the folder
// without touching the other profiles' modlist.txt orphans the entry there:
// the mod comes back as an unmanaged "new mod" and loses both its load order
// and its enabled state. This proves the rewrite reaches every profile,
// preserves line order and the '+'/'-'/'*' spec, and leaves the rest alone.
TEST_CASE("rename_mod_in_all_profiles rewrites every profile's modlist", "[engine]") {
  auto root               = make_temp_dir("rename_mod");
  const auto profiles_dir = root / "profiles";

  const std::string alpha_before =
      "+SkyUI\n-SomeOtherMod\n+Old Mod Name\n*Foreign Mod\n# a comment\n";
  const std::string beta_before  = "+Old Mod Name\n-Disabled One\n";
  const std::string gamma_before = "+Totally Unrelated\n";

  REQUIRE(engine::profile::create_fresh_profile(profiles_dir, "Alpha").success);
  REQUIRE(engine::profile::create_fresh_profile(profiles_dir, "Beta").success);
  REQUIRE(engine::profile::create_fresh_profile(profiles_dir, "Gamma").success);
  write_text(profiles_dir / "Alpha" / "modlist.txt", alpha_before);
  write_text(profiles_dir / "Beta" / "modlist.txt", beta_before);
  write_text(profiles_dir / "Gamma" / "modlist.txt", gamma_before);

  const auto outcome = engine::profile::rename_mod_in_all_profiles(
      profiles_dir, "Old Mod Name", "New Mod Name");
  // Alpha and Beta reference the mod; Gamma does not and is skipped.
  REQUIRE(outcome.rewritten == 2);
  REQUIRE(outcome.failed.empty());

  // Alpha: the name moved, and the load-order line positions, the disabled
  // line and the comment are all byte-for-byte as they were.
  const std::string alpha_after = read_text(profiles_dir / "Alpha" / "modlist.txt");
  REQUIRE(alpha_after ==
          "+SkyUI\n-SomeOtherMod\n+New Mod Name\n*Foreign Mod\n# a comment\n");

  // Beta: the spec character survives the rewrite.
  const std::string beta_after = read_text(profiles_dir / "Beta" / "modlist.txt");
  REQUIRE(beta_after == "+New Mod Name\n-Disabled One\n");

  // Gamma never mentioned the mod and is left exactly as it was.
  REQUIRE(read_text(profiles_dir / "Gamma" / "modlist.txt") == gamma_before);

  // A rename to the same name, or of a mod no profile lists, changes nothing.
  REQUIRE(engine::profile::rename_mod_in_all_profiles(profiles_dir, "New Mod Name",
                                                      "New Mod Name")
              .rewritten == 0);
  REQUIRE(engine::profile::rename_mod_in_all_profiles(profiles_dir, "No Such Mod",
                                                      "Whatever")
              .rewritten == 0);
  REQUIRE(read_text(profiles_dir / "Alpha" / "modlist.txt") == alpha_after);
}

// The round trip through ProfileManager is what the user actually gets: after
// the rewrite, loading the profile resolves the mod under its new name and
// keeps the priority its line position encoded. Without the rewrite the old
// name is gone from the file and the mod re-enters as a new unmanaged mod with
// default priority and enabled state.
TEST_CASE("a renamed mod keeps its priority and enabled state in another profile",
          "[engine]") {
  auto root               = make_temp_dir("rename_priority");
  const auto profiles_dir = root / "profiles";
  REQUIRE(engine::profile::create_fresh_profile(profiles_dir, "Other").success);
  // Highest priority first. The mod under rename is disabled and sits in the
  // middle, so its encoded priority is the middle of three.
  write_text(profiles_dir / "Other" / "modlist.txt",
             "+Top Mod\n-Old Mod Name\n+Bottom Mod\n");

  engine::profile::ProfileManager profile(profiles_dir / "Other");
  profile.refresh_mod_status({"Top Mod", "Old Mod Name", "Bottom Mod"}, {});
  REQUIRE(engine::profile::rename_mod_in_all_profiles(profiles_dir, "Old Mod Name",
                                                      "New Mod Name")
              .rewritten == 1);

  // Reload from the rewritten file under the new name.
  engine::profile::ProfileManager reloaded(profiles_dir / "Other");
  reloaded.refresh_mod_status({"Top Mod", "New Mod Name", "Bottom Mod"}, {});
  REQUIRE(reloaded.priority_of("New Mod Name") == 1);
  const auto mods = reloaded.mods();
  REQUIRE(mods.size() == 3);
  bool found = false;
  for (const auto &m : mods) {
    if (m.mod_id == "New Mod Name") {
      found = true;
      REQUIRE_FALSE(m.enabled);  // the '-' spec survived
    }
  }
  REQUIRE(found);
}