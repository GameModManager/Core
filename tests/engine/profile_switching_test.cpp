// Engine test for profile switching (engine/profile/profile_switching) —
// save_current_profile (modlist immediate flush, archives.txt, tweaked INI,
// settings.ini), switch_profile (full transition: save current, construct new
// Profile, restore mod/plugin state, archive invalidation, refresh callbacks,
// profile_changed event), and write_tweaked_ini. Uses temp dirs only, no Qt.
#include "engine/core/events/event_bus.h"
#include "engine/game/plugins/plugin_database.h"
#include "engine/profile/profile.h"
#include "engine/profile/profile_creation.h"
#include "engine/profile/profile_switching.h"

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
using namespace std::chrono_literals;

namespace {

std::atomic<int> g_counter{0};

fs::path make_temp_dir(const char *tag) {
  auto dir =
      fs::temp_directory_path() /
      ("gmm_profile_switch_" + std::string(tag) + "_" +
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

// Minimal valid TES4 plugin file (no masters) so PluginDatabase::refresh
// discovers it.
void write_esp(const fs::path &path) {
  std::ofstream out(path, std::ios::binary);
  out.write("TES4", 4);
  const uint32_t data_size = 0;
  out.write(reinterpret_cast<const char *>(&data_size), 4);
  const uint32_t zero = 0;
  out.write(reinterpret_cast<const char *>(&zero), 4);
  out.write(reinterpret_cast<const char *>(&zero), 4);
  out.write(reinterpret_cast<const char *>(&zero), 4);
  out.write(reinterpret_cast<const char *>(&zero), 4);
}

// Build a plugin database with two plugins (Alpha.esp, Beta.esp) from a fake
// game dir. The DB is the live plugin state the switcher saves/restores.
engine::PluginDatabase make_plugin_db(const fs::path &game_dir,
                                      const fs::path &mods_dir,
                                      const fs::path &meta_dir) {
  fs::create_directories(game_dir / "Data");
  write_esp(game_dir / "Data" / "Alpha.esp");
  write_esp(game_dir / "Data" / "Beta.esp");
  engine::PluginDatabase db;
  REQUIRE(db.refresh(game_dir, mods_dir, meta_dir, "", ""));
  db.sort_load_order();
  db.set_all_enabled();
  db.generate_mod_indexes();
  return db;
}

// Recording callbacks: every invocation appends to `log` so tests can assert
// the MO2 setCurrentProfile ordering.
engine::profile::ProfileSwitchCallbacks
recording_callbacks(std::vector<std::string> &log,
                    bool *invalidation_active = nullptr) {
  engine::profile::ProfileSwitchCallbacks cb;
  cb.refresh_directory_structure = [&log] {
    log.push_back("refresh_directory_structure");
  };
  cb.refresh_plugin_list = [&log] { log.push_back("refresh_plugin_list"); };
  cb.refresh_bsa_list = [&log] { log.push_back("refresh_bsa_list"); };
  cb.set_archive_invalidation = [&log, invalidation_active](bool active) {
    log.push_back(std::string("set_archive_invalidation:") +
                  (active ? "1" : "0"));
    if (invalidation_active) {
      *invalidation_active = active;
    }
  };
  return cb;
}

} // namespace

// ---------------------------------------------------------------------------
// save_current_profile
// ---------------------------------------------------------------------------

TEST_CASE("save_current_profile flushes modlist and writes archives/settings",
          "[engine]") {
  auto root = make_temp_dir("save");
  const auto profiles_dir = root / "profiles";
  auto created = engine::profile::create_fresh_profile(profiles_dir, "Default");
  REQUIRE(created.success);

  engine::profile::Profile profile(created.directory,
                                   std::chrono::milliseconds(10s));
  profile.refresh_mod_status({"ModA", "ModB"});
  profile.set_mod_enabled("ModA", false); // schedules a delayed write

  engine::profile::ProfileSaveState state;
  state.known_mods = {"ModA", "ModB"};
  state.archives = {"Skyrim - Textures.bsa"};
  state.tweaked_ini = "[Archive]\nbInvalidateOlderFiles=1\n";

  std::string error;
  REQUIRE(
      engine::profile::save_current_profile(profile, state, nullptr, &error));
  REQUIRE(error.empty());

  // modlist.txt flushed immediately (the delayed write was pending).
  const std::string modlist = read_text(created.directory / "modlist.txt");
  REQUIRE(modlist.find("-ModA") != std::string::npos);
  REQUIRE(modlist.find("+ModB") != std::string::npos);

  // archives.txt + initweaks.ini + settings.ini written.
  REQUIRE(read_text(created.directory / "archives.txt")
              .find("Skyrim - Textures.bsa") != std::string::npos);
  REQUIRE(read_text(created.directory / "initweaks.ini")
              .find("bInvalidateOlderFiles=1") != std::string::npos);
  REQUIRE(fs::exists(created.directory / "settings.ini"));
}

TEST_CASE("save_current_profile skips tweaked ini when empty", "[engine]") {
  auto root = make_temp_dir("save_no_tweak");
  const auto profiles_dir = root / "profiles";
  auto created = engine::profile::create_fresh_profile(profiles_dir, "Default");
  REQUIRE(created.success);

  engine::profile::Profile profile(created.directory);
  engine::profile::ProfileSaveState state;
  state.known_mods = {"ModA"};
  state.archives = {};

  std::string error;
  REQUIRE(
      engine::profile::save_current_profile(profile, state, nullptr, &error));
  REQUIRE_FALSE(fs::exists(created.directory / "initweaks.ini"));
  // archives.txt is always written (empty list is a valid state).
  REQUIRE(fs::exists(created.directory / "archives.txt"));
}

// Regression for the "profile switch wipes modlist.txt" P0 bug: the Profile
// constructor only loads settings.ini — mods_ is empty until
// refresh_mod_status() is called. save_current_profile() must never flush
// that empty list over a populated modlist.txt (the UI used to construct the
// current profile without loading it, wiping every profile's enabled state on
// switch).
TEST_CASE("save_current_profile preserves an unloaded profile's modlist",
          "[engine]") {
  auto root = make_temp_dir("save_unloaded");
  const auto profiles_dir = root / "profiles";
  auto created = engine::profile::create_fresh_profile(profiles_dir, "Default");
  REQUIRE(created.success);

  // Populate the profile's modlist.txt on disk: ModA enabled, ModB disabled.
  {
    engine::profile::Profile p(created.directory);
    p.refresh_mod_status({"ModA", "ModB"});
    p.set_mod_enabled("ModB", false);
    p.write_modlist_now();
  }
  REQUIRE(read_text(created.directory / "modlist.txt").find("-ModB") !=
          std::string::npos);

  // A fresh Profile (never loaded — mods_ empty) must not wipe the file.
  engine::profile::Profile profile(created.directory);
  engine::profile::ProfileSaveState state;
  state.known_mods = {"ModA", "ModB"};
  std::string error;
  REQUIRE(
      engine::profile::save_current_profile(profile, state, nullptr, &error));
  REQUIRE(error.empty());

  const std::string modlist = read_text(created.directory / "modlist.txt");
  REQUIRE(modlist.find("-ModB") != std::string::npos);
  REQUIRE(modlist.find("+ModA") != std::string::npos);
}

TEST_CASE("write_tweaked_ini writes atomically", "[engine]") {
  auto dir = make_temp_dir("tweak");
  std::string error;
  REQUIRE(engine::profile::write_tweaked_ini(
      dir, "[Archive]\nbInvalidateOlderFiles=1\n", &error));
  REQUIRE(error.empty());
  REQUIRE(read_text(dir / "initweaks.ini").find("bInvalidateOlderFiles=1") !=
          std::string::npos);
}

// ---------------------------------------------------------------------------
// switch_profile — no-op / error paths
// ---------------------------------------------------------------------------

TEST_CASE("switch_profile is a no-op for the current profile", "[engine]") {
  auto root = make_temp_dir("noop");
  const auto profiles_dir = root / "profiles";
  auto created = engine::profile::create_fresh_profile(profiles_dir, "Default");
  REQUIRE(created.success);

  engine::profile::Profile current(created.directory);
  std::vector<std::string> log;
  auto result = engine::profile::switch_profile(
      profiles_dir, "Default", &current, {}, nullptr, recording_callbacks(log));

  REQUIRE(result.success);
  REQUIRE_FALSE(result.changed);
  REQUIRE(result.profile == nullptr);
  REQUIRE(log.empty()); // nothing was refreshed
}

TEST_CASE("switch_profile fails for a missing profile", "[engine]") {
  auto root = make_temp_dir("missing");
  const auto profiles_dir = root / "profiles";
  fs::create_directories(profiles_dir);

  std::vector<std::string> log;
  auto result = engine::profile::switch_profile(
      profiles_dir, "Nope", nullptr, {}, nullptr, recording_callbacks(log));

  REQUIRE_FALSE(result.success);
  REQUIRE_FALSE(result.changed);
  REQUIRE(result.profile == nullptr);
  REQUIRE_FALSE(result.error.empty());
  REQUIRE(log.empty());
}

// ---------------------------------------------------------------------------
// switch_profile — full transition
// ---------------------------------------------------------------------------

TEST_CASE("switch_profile saves current, restores mod state and refreshes",
          "[engine]") {
  auto root = make_temp_dir("switch");
  const auto profiles_dir = root / "profiles";

  // Profile A: ModA enabled (high), ModB disabled (low).
  auto a = engine::profile::create_fresh_profile(profiles_dir, "Alpha");
  REQUIRE(a.success);
  {
    engine::profile::Profile pa(a.directory);
    pa.refresh_mod_status({"ModA", "ModB"});
    pa.set_mod_enabled("ModB", false);
    pa.write_modlist_now();
    pa.set_automatic_archive_invalidation(true);
    REQUIRE(pa.save_settings());
  }

  // Profile B: ModB enabled (high), ModA disabled (low), no invalidation.
  auto b = engine::profile::create_fresh_profile(profiles_dir, "Beta");
  REQUIRE(b.success);
  {
    engine::profile::Profile pb(b.directory);
    pb.refresh_mod_status({"ModA", "ModB"});
    pb.set_mod_enabled("ModA", false);
    pb.write_modlist_now();
    pb.set_automatic_archive_invalidation(false);
    REQUIRE(pb.save_settings());
  }

  // Current profile is Alpha with a pending (unflushed) modlist change.
  engine::profile::Profile current(a.directory, std::chrono::milliseconds(10s));
  current.refresh_mod_status({"ModA", "ModB"});
  current.set_mod_enabled("ModA", false); // pending delayed write

  engine::profile::ProfileSaveState state;
  state.known_mods = {"ModA", "ModB"};
  state.archives = {"Alpha.bsa"};

  std::vector<std::string> log;
  bool invalidation_active = true;
  auto result = engine::profile::switch_profile(
      profiles_dir, "beta", &current, state, nullptr,
      recording_callbacks(log,
                          &invalidation_active)); // lowercase name on purpose

  REQUIRE(result.success);
  REQUIRE(result.changed);
  REQUIRE(result.profile != nullptr);
  REQUIRE(result.profile->directory() == b.directory);

  // The current profile was saved before switching: the pending modlist
  // change was flushed and archives.txt written.
  REQUIRE(read_text(a.directory / "modlist.txt").find("-ModA") !=
          std::string::npos);
  REQUIRE(read_text(a.directory / "archives.txt").find("Alpha.bsa") !=
          std::string::npos);

  // Mod state restored from Beta's modlist.txt: ModB enabled (high), ModA
  // disabled (low).
  const auto mods = result.profile->mods();
  REQUIRE(mods.size() == 2);
  REQUIRE(mods[0].mod_id == "ModA");
  REQUIRE_FALSE(mods[0].enabled);
  REQUIRE(mods[1].mod_id == "ModB");
  REQUIRE(mods[1].enabled);

  // Archive invalidation deactivated (Beta has AutomaticArchiveInvalidation
  // = false) and the refresh callbacks ran in MO2 order.
  REQUIRE_FALSE(invalidation_active);
  REQUIRE(log ==
          std::vector<std::string>(
              {"set_archive_invalidation:0", "refresh_directory_structure",
               "refresh_plugin_list", "refresh_bsa_list"}));
}

// Regression for the "profile switch wipes modlist.txt" P0 bug: the UI used
// to construct the current Profile without loading it (the ctor only reads
// settings.ini), so switch_profile() flushed an empty modlist.txt over the
// current profile's real per-mod state. The switch must preserve the current
// profile's modlist.txt even when the caller never called refresh_mod_status.
TEST_CASE("switch_profile preserves the current profile's modlist when "
          "unloaded",
          "[engine]") {
  auto root = make_temp_dir("switch_unloaded");
  const auto profiles_dir = root / "profiles";
  auto a = engine::profile::create_fresh_profile(profiles_dir, "Alpha");
  auto b = engine::profile::create_fresh_profile(profiles_dir, "Beta");
  REQUIRE(a.success);
  REQUIRE(b.success);

  // Alpha's modlist.txt on disk: ModA enabled, ModB disabled.
  {
    engine::profile::Profile pa(a.directory);
    pa.refresh_mod_status({"ModA", "ModB"});
    pa.set_mod_enabled("ModB", false);
    pa.write_modlist_now();
  }
  REQUIRE(read_text(a.directory / "modlist.txt").find("-ModB") !=
          std::string::npos);

  // Current Profile constructed WITHOUT loading (the UI bug: mods_ empty).
  engine::profile::Profile current(a.directory);
  engine::profile::ProfileSaveState state;
  state.known_mods = {"ModA", "ModB"};

  std::vector<std::string> log;
  auto result = engine::profile::switch_profile(
      profiles_dir, "Beta", &current, state, nullptr, recording_callbacks(log));
  REQUIRE(result.success);
  REQUIRE(result.changed);

  // Alpha's modlist.txt must still carry its per-profile state.
  const std::string alpha_modlist = read_text(a.directory / "modlist.txt");
  REQUIRE(alpha_modlist.find("-ModB") != std::string::npos);
  REQUIRE(alpha_modlist.find("+ModA") != std::string::npos);

  // Beta's restored state: its own (empty) modlist.txt converged with the
  // known mods — both enabled by default (MO2's refreshModStatus).
  REQUIRE(result.profile != nullptr);
  const auto beta_mods = result.profile->mods();
  REQUIRE(beta_mods.size() == 2);
  REQUIRE(beta_mods[0].mod_id == "ModA");
  REQUIRE(beta_mods[0].enabled);
  REQUIRE(beta_mods[1].mod_id == "ModB");
  REQUIRE(beta_mods[1].enabled);
}

TEST_CASE("switch_profile emits profile_changed event", "[engine]") {
  auto root = make_temp_dir("event");
  const auto profiles_dir = root / "profiles";
  auto a = engine::profile::create_fresh_profile(profiles_dir, "Alpha");
  auto b = engine::profile::create_fresh_profile(profiles_dir, "Beta");
  REQUIRE(a.success);
  REQUIRE(b.success);

  std::string received_event;
  std::string received_payload;
  const auto token = engine::EventBus::instance().subscribe(
      engine::events::kProfileChanged,
      [&](const std::string &event_id, const std::string &payload) {
        received_event = event_id;
        received_payload = payload;
      });

  engine::profile::Profile current(a.directory);
  engine::profile::ProfileSaveState state;
  state.known_mods = {"ModA"};
  std::vector<std::string> log;
  auto result = engine::profile::switch_profile(
      profiles_dir, "Beta", &current, state, nullptr, recording_callbacks(log));

  REQUIRE(result.success);
  REQUIRE(received_event == engine::events::kProfileChanged);
  REQUIRE(received_payload.find("\"profile\":\"Beta\"") != std::string::npos);
  REQUIRE(received_payload.find("\"old_profile\":\"Alpha\"") !=
          std::string::npos);

  engine::EventBus::instance().unsubscribe(token);
}

// ---------------------------------------------------------------------------
// switch_profile — plugin state round-trip through PluginDatabase
// ---------------------------------------------------------------------------

TEST_CASE("switch_profile saves and restores plugin state via PluginDatabase",
          "[engine]") {
  auto root = make_temp_dir("plugins");
  const auto profiles_dir = root / "profiles";
  const auto game_dir = root / "game";
  const auto mods_dir = root / "mods";
  const auto meta_dir = root / "meta";
  fs::create_directories(profiles_dir);
  fs::create_directories(mods_dir);
  fs::create_directories(meta_dir);

  auto a = engine::profile::create_fresh_profile(profiles_dir, "Alpha");
  auto b = engine::profile::create_fresh_profile(profiles_dir, "Beta");
  REQUIRE(a.success);
  REQUIRE(b.success);

  // Live plugin DB: both plugins enabled, Alpha.esp first.
  auto db = make_plugin_db(game_dir, mods_dir, meta_dir);
  REQUIRE(db.plugins().size() == 2);
  REQUIRE(db.set_enabled("Beta.esp", false)); // Beta disabled in Alpha

  // Save Alpha's plugin state through the switcher.
  engine::profile::Profile current(a.directory);
  engine::profile::ProfileSaveState state;
  state.known_mods = {};
  std::string error;
  REQUIRE(engine::profile::save_current_profile(current, state, &db, &error));
  REQUIRE(error.empty());

  // Alpha's plugins.txt reflects the live state (Beta disabled).
  const std::string alpha_plugins = read_text(a.directory / "plugins.txt");
  REQUIRE(alpha_plugins.find("*Alpha.esp") != std::string::npos);
  REQUIRE(alpha_plugins.find("Beta.esp") != std::string::npos);
  REQUIRE(alpha_plugins.find("*Beta.esp") == std::string::npos);

  // Beta enables everything (fresh profile default): the user toggles Beta
  // back on in the UI, then the profile is saved.
  REQUIRE(db.set_enabled("Beta.esp", true));
  {
    engine::profile::Profile pb(b.directory);
    engine::profile::ProfileSaveState bstate;
    bstate.known_mods = {};
    REQUIRE(engine::profile::save_current_profile(pb, bstate, &db, &error));
  }
  REQUIRE(read_text(b.directory / "plugins.txt").find("*Beta.esp") !=
          std::string::npos);

  // Switch to Beta: the live DB must now reflect Beta's plugin state.
  std::vector<std::string> log;
  auto result = engine::profile::switch_profile(
      profiles_dir, "Beta", &current, state, &db, recording_callbacks(log));
  REQUIRE(result.success);

  const auto *beta_plugin = db.find("Beta.esp");
  REQUIRE(beta_plugin != nullptr);
  REQUIRE(beta_plugin->enabled);
  const auto *alpha_plugin = db.find("Alpha.esp");
  REQUIRE(alpha_plugin != nullptr);
  REQUIRE(alpha_plugin->enabled);
}