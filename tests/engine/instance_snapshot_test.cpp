// Engine test for comprehensive instance state snapshot.
//
// Covers Workspace-2z2n: InstanceSnapshot aggregates ALL per-instance
// tracked state (mod entries, profile state, executables) into a single
// serializable object. Round-trip through JSON, capture from disk, apply to disk.

#include "engine/core/instance/instance_snapshot.h"

#include "engine/gmmpack/uuid.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;

namespace {
std::string read_text(const fs::path &p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void write_text(const fs::path &p, const std::string &content) {
  fs::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << content;
}

fs::path test_root(const char *name) {
  return fs::path("/tmp/gmm_snapshot_test") / name;
}

void require(bool cond, const char *msg) {
  INFO(msg);
  REQUIRE(cond);
}
}  // namespace

// ---------------------------------------------------------------------------
// JSON round-trip: to_json <-> from_json preserves all fields
// ---------------------------------------------------------------------------

TEST_CASE("snapshot - JSON round-trip preserves all fields", "[engine]") {
  using engine::InstanceSnapshot;

  InstanceSnapshot snap;
  snap.game_id         = "SkyrimSpecialEdition";
  snap.display_name    = "My Skyrim";
  snap.portable        = true;
  snap.deploy_strategy = "symlink";
  snap.proton_runner   = "GE-Proton9-12";
  snap.steam_appid     = 489830;

  // Mod entries.
  engine::ModTrackingEntry e1;
  e1.install_order         = 1;
  e1.installed_at          = 1234567890;
  e1.list_position         = 0;
  e1.depth                 = 0;
  e1.hidden                = true;
  snap.mod_entries["SKSE"] = e1;

  engine::ModTrackingEntry e2;
  e2.install_order        = 2;
  e2.installed_at         = 1234567891;
  e2.list_position        = 1;
  e2.parent_separator     = "Graphics";
  e2.depth                = 1;
  e2.separator_color      = "#ff0000";
  e2.collapsed            = true;
  snap.mod_entries["ENB"] = e2;

  engine::ModTrackingEntry e3;
  e3.install_order          = 3;
  e3.list_position          = 2;
  e3.disabled               = true;
  e3.deploy_at_root         = true;
  e3.hidden_files           = {"d3d11.dll"};
  snap.mod_entries["USSEP"] = e3;

  snap.next_install_order = 4;

  // Profile.
  engine::ProfileSnapshot ps;
  ps.name = "Default";
  ps.mods = {
      {"ModA", true, false, 2}, {"ModB", false, false, 1}, {"DLC1", true, true, 0}};
  ps.plugins                   = {"Skyrim.esm", "Update.esm"};
  ps.load_order                = {"Update.esm", "Skyrim.esm"};
  ps.locked_order              = {{"Skyrim.esm", 0}};
  ps.archives                  = {"Skyrim - Textures.bsa"};
  ps.local_saves               = true;
  ps.auto_archive_invalidation = true;
  ps.tweaked_ini               = "[Display]\niSize W=1920\n";
  snap.profiles.push_back(ps);

  // Executables.
  engine::ExecutableEntry exec;
  exec.path  = "SkyrimSE.exe";
  exec.title = "Skyrim";
  exec.args  = "-美学 testing";
  exec.cwd   = "";
  exec.mod   = "";
  exec.icon  = "icon.ico";
  exec.env   = {"VAR1=val1", "VAR2=val2"};
  snap.executables.push_back(exec);

  // Round-trip.
  auto j        = snap.to_json();
  auto restored = InstanceSnapshot::from_json(j);

  require(restored.game_id == "SkyrimSpecialEdition", "game_id");
  require(restored.display_name == "My Skyrim", "display_name");
  require(restored.portable == true, "portable");
  require(restored.deploy_strategy == "symlink", "deploy_strategy");
  require(restored.proton_runner == "GE-Proton9-12", "proton_runner");
  require(restored.steam_appid == 489830, "steam_appid");

  require(restored.mod_entries.size() == 3, "mod_entries count");
  require(restored.mod_entries["SKSE"].hidden == true, "SKSE hidden");
  require(restored.mod_entries["ENB"].separator_color == "#ff0000", "ENB color");
  require(restored.mod_entries["ENB"].collapsed == true, "ENB collapsed");
  require(restored.mod_entries["USSEP"].disabled == true, "USSEP disabled");
  require(restored.mod_entries["USSEP"].deploy_at_root == true, "USSEP deploy_at_root");
  require(restored.mod_entries["USSEP"].hidden_files.size() == 1, "USSEP hidden_files");
  require(restored.next_install_order == 4, "next_install_order");

  require(restored.profiles.size() == 1, "profiles count");
  require(restored.profiles[0].name == "Default", "profile name");
  require(restored.profiles[0].mods.size() == 3, "mod count");
  require(restored.profiles[0].mods[0].mod_id == "ModA", "mod A id");
  require(restored.profiles[0].mods[0].enabled == true, "mod A enabled");
  require(restored.profiles[0].mods[1].enabled == false, "mod B disabled");
  require(restored.profiles[0].mods[2].foreign == true, "DLC1 foreign");
  require(restored.profiles[0].plugins.size() == 2, "plugins count");
  require(restored.profiles[0].load_order.size() == 2, "load_order count");
  require(restored.profiles[0].locked_order.size() == 1, "locked_order count");
  require(restored.profiles[0].archives.size() == 1, "archives count");
  require(restored.profiles[0].local_saves == true, "local_saves");
  require(restored.profiles[0].auto_archive_invalidation == true,
          "auto_archive_invalidation");
  require(restored.profiles[0].tweaked_ini == "[Display]\niSize W=1920\n",
          "tweaked_ini");

  require(restored.executables.size() == 1, "executables count");
  require(restored.executables[0].path == "SkyrimSE.exe", "exec path");
  require(restored.executables[0].title == "Skyrim", "exec title");
  require(restored.executables[0].args == "-美学 testing", "exec args");
  require(restored.executables[0].icon == "icon.ico", "exec icon");
  require(restored.executables[0].env.size() == 2, "exec env size");
}

// ---------------------------------------------------------------------------
// save/load file round-trip
// ---------------------------------------------------------------------------

TEST_CASE("snapshot - save and load file round-trip", "[engine]") {
  using engine::InstanceSnapshot;

  const auto root = test_root("file_roundtrip");
  fs::remove_all(root);
  fs::create_directories(root);

  const auto path = root / "snapshot.json";

  InstanceSnapshot snap;
  snap.game_id      = "SkyrimSpecialEdition";
  snap.display_name = "Test Instance";
  snap.portable     = true;
  snap.steam_appid  = 489830;

  engine::ModTrackingEntry e;
  e.install_order          = 1;
  e.list_position          = 0;
  snap.mod_entries["SKSE"] = e;
  snap.next_install_order  = 2;

  engine::ProfileSnapshot ps;
  ps.name    = "Default";
  ps.mods    = {{"ModA", true, false, 0}};
  ps.plugins = {"Skyrim.esm"};
  snap.profiles.push_back(ps);

  require(snap.save(path), "save succeeds");
  require(fs::exists(path), "file created");

  auto loaded = InstanceSnapshot::load(path);
  require(loaded.game_id == "SkyrimSpecialEdition", "game_id loaded");
  require(loaded.display_name == "Test Instance", "display_name loaded");
  require(loaded.steam_appid == 489830, "steam_appid loaded");
  require(loaded.mod_entries.size() == 1, "mod_entries loaded");
  require(loaded.profiles.size() == 1, "profiles loaded");
  require(loaded.profiles[0].mods.size() == 1, "mods loaded");

  fs::remove_all(root);
}

// ---------------------------------------------------------------------------
// load returns empty snapshot on bad/missing file
// ---------------------------------------------------------------------------

TEST_CASE("snapshot - load handles missing and corrupt files", "[engine]") {
  using engine::InstanceSnapshot;

  const auto root = test_root("bad_file");
  fs::remove_all(root);
  fs::create_directories(root);

  // Missing file returns empty.
  auto s1 = InstanceSnapshot::load(root / "nope.json");
  require(s1.game_id.empty(), "missing file returns empty snapshot");

  // Corrupt file returns empty.
  write_text(root / "corrupt.json", "not json at all");
  auto s2 = InstanceSnapshot::load(root / "corrupt.json");
  require(s2.game_id.empty(), "corrupt file returns empty snapshot");

  fs::remove_all(root);
}

// ---------------------------------------------------------------------------
// capture from a real instance directory
// ---------------------------------------------------------------------------

TEST_CASE("snapshot - capture from disk instance", "[engine]") {
  using engine::Instance;
  using engine::InstanceSnapshot;
  using engine::ModStateTracker;
  using engine::profile::ModListEntry;

  const auto root = test_root("capture");
  fs::remove_all(root);
  fs::create_directories(root);

  // Write instance.toml.
  write_text(root / "instance.toml", "game_id = \"SkyrimSpecialEdition\"\n"
                                     "name = \"Capture Test\"\n"
                                     "portable = true\n"
                                     "steam_appid = 489830\n"
                                     "deploy_strategy = \"symlink\"\n");

  // Write mod_state.json.
  {
    ModStateTracker tracker(root);
    tracker.record_install("SKSE");
    tracker.set_position("SKSE", 0);
    tracker.set_hidden("SKSE", true);
    tracker.record_install("USSEP");
    tracker.set_position("USSEP", 1);
    tracker.set_deploy_at_root("USSEP", true);
    tracker.set_separator_visual("Graphics", "#aabbcc", true);
    tracker.set_position("Graphics", 2);
    tracker.set_nesting("ENB", "Graphics", 1);
    tracker.set_hidden_files("ENB", {"d3d11.dll"});
    require(tracker.save(), "mod_state saved");
  }

  // Create profiles/Default/ with files.
  auto prof = root / "profiles" / "Default";
  fs::create_directories(prof);
  write_text(prof / "modlist.txt", "+SKSE\r\n-USSEP\r\n*DLC1\r\n");
  write_text(prof / "plugins.txt", "Skyrim.esm\nUpdate.esm\n");
  write_text(prof / "loadorder.txt", "Update.esm\nSkyrim.esm\n");
  write_text(prof / "lockedorder.txt", "Skyrim.esm|0\n");
  write_text(prof / "archives.txt", "Skyrim - Textures.bsa\n");
  write_text(prof / "settings.ini", "LocalSaves=true\nLocalSettings=false\n"
                                    "AutomaticArchiveInvalidation=true\n");
  write_text(prof / "initweaks.ini", "[Display]\niSize W=1920\n");

  // Capture.
  Instance inst = Instance::from_root(root);
  auto snap     = InstanceSnapshot::capture(inst);

  require(snap.game_id == "SkyrimSpecialEdition", "captured game_id");
  require(snap.display_name == "Capture Test", "captured display_name");
  require(snap.portable == true, "captured portable");
  require(snap.deploy_strategy == "symlink", "captured deploy_strategy");
  require(snap.steam_appid == 489830, "captured steam_appid");

  // Mod entries.
  require(snap.mod_entries.size() == 4, "captured 4 mod entries");
  require(snap.mod_entries["SKSE"].hidden == true, "SKSE hidden captured");
  require(snap.mod_entries["SKSE"].install_order == 1, "SKSE install_order captured");
  require(snap.mod_entries["USSEP"].deploy_at_root == true,
          "USSEP deploy_at_root captured");
  require(snap.mod_entries["Graphics"].separator_color == "#aabbcc",
          "separator color captured");
  require(snap.mod_entries["Graphics"].collapsed == true,
          "separator collapsed captured");
  require(snap.mod_entries["ENB"].parent_separator == "Graphics",
          "ENB parent captured");
  require(snap.mod_entries["ENB"].depth == 1, "ENB depth captured");
  require(snap.mod_entries["ENB"].hidden_files.size() == 1,
          "ENB hidden_files captured");

  // Profile.
  require(snap.profiles.size() == 1, "captured 1 profile");
  require(snap.profiles[0].name == "Default", "profile name captured");
  auto &pm = snap.profiles[0].mods;
  require(pm.size() == 3, "captured 3 mods in profile");
  // Modlist parsing: file order is +SKSE, -USSEP, *DLC1.
  // Priorities: SKSE=2(highest), USSEP=1, DLC1=0(lowest).
  bool found_skse = false, found_ussep = false, found_dlc = false;
  for (const auto &m : pm) {
    if (m.mod_id == "SKSE") {
      found_skse = true;
      require(m.enabled == true, "SKSE enabled in profile");
      require(m.priority == 2, "SKSE priority highest");
    } else if (m.mod_id == "USSEP") {
      found_ussep = true;
      require(m.enabled == false, "USSEP disabled in profile");
      require(m.priority == 1, "USSEP priority mid");
    } else if (m.mod_id == "DLC1") {
      found_dlc = true;
      require(m.foreign == true, "DLC1 foreign");
      require(m.priority == 0, "DLC1 priority lowest");
    }
  }
  require(found_skse, "SKSE found in profile mods");
  require(found_ussep, "USSEP found in profile mods");
  require(found_dlc, "DLC1 found in profile mods");

  require(snap.profiles[0].plugins.size() == 2, "plugins captured");
  require(snap.profiles[0].load_order.size() == 2, "load_order captured");
  require(snap.profiles[0].locked_order.size() == 1, "locked_order captured");
  require(snap.profiles[0].locked_order[0].name == "Skyrim.esm",
          "locked name captured");
  require(snap.profiles[0].locked_order[0].priority == 0, "locked priority captured");
  require(snap.profiles[0].archives.size() == 1, "archives captured");
  require(snap.profiles[0].local_saves == true, "local_saves captured");
  require(snap.profiles[0].auto_archive_invalidation == true,
          "auto_archive_invalidation captured");
  require(snap.profiles[0].tweaked_ini == "[Display]\niSize W=1920\n",
          "tweaked_ini captured");

  fs::remove_all(root);
}

// ---------------------------------------------------------------------------
// apply writes a full instance and capture re-reads it
// ---------------------------------------------------------------------------

TEST_CASE("snapshot - apply then capture is idempotent", "[engine]") {
  using engine::Instance;
  using engine::InstanceSnapshot;
  using engine::ModTrackingEntry;
  using engine::ProfileSnapshot;

  const auto root = test_root("apply_capture");
  fs::remove_all(root);
  fs::create_directories(root);

  // Build a snapshot.
  InstanceSnapshot snap;
  snap.game_id         = "SkyrimSpecialEdition";
  snap.display_name    = "Apply Test";
  snap.portable        = true;
  snap.steam_appid     = 489830;
  snap.deploy_strategy = "symlink";
  snap.proton_runner   = "GE-Proton9";

  ModTrackingEntry e1;
  e1.install_order         = 1;
  e1.installed_at          = 1000000;
  e1.list_position         = 0;
  e1.depth                 = 0;
  snap.mod_entries["SKSE"] = e1;

  ModTrackingEntry e2;
  e2.install_order        = 2;
  e2.installed_at         = 1000001;
  e2.list_position        = 1;
  e2.parent_separator     = "Graphics";
  e2.depth                = 1;
  e2.deploy_at_root       = true;
  e2.hidden_files         = {"d3d11.dll"};
  snap.mod_entries["ENB"] = e2;

  ModTrackingEntry sep;
  sep.install_order            = 3;
  sep.list_position            = 2;
  sep.separator_color          = "#aabbcc";
  sep.collapsed                = true;
  snap.mod_entries["Graphics"] = sep;

  snap.next_install_order = 4;

  ProfileSnapshot ps;
  ps.name                      = "Default";
  ps.mods                      = {{"SKSE", true, false, 1}, {"ENB", true, false, 0}};
  ps.plugins                   = {"Skyrim.esm"};
  ps.load_order                = {"Skyrim.esm"};
  ps.locked_order              = {{"Skyrim.esm", 0}};
  ps.archives                  = {"Skyrim - Textures.bsa"};
  ps.local_saves               = true;
  ps.local_settings            = false;
  ps.auto_archive_invalidation = true;
  ps.tweaked_ini               = "[Display]\niSize W=1920\n";
  snap.profiles.push_back(ps);

  // Apply to disk.
  Instance inst = Instance::from_root(root);
  require(snap.apply(inst), "apply succeeds");

  // Verify files exist.
  require(fs::exists(root / "instance.toml"), "instance.toml written");
  require(fs::exists(root / "mod_state.json"), "mod_state.json written");
  require(fs::exists(root / "profiles" / "Default" / "modlist.txt"),
          "modlist.txt written");
  require(fs::exists(root / "profiles" / "Default" / "plugins.txt"),
          "plugins.txt written");
  require(fs::exists(root / "profiles" / "Default" / "settings.ini"),
          "settings.ini written");
  require(fs::exists(root / "profiles" / "Default" / "initweaks.ini"),
          "initweaks.ini written");

  // Capture back.
  auto captured = InstanceSnapshot::capture(inst);

  require(captured.game_id == snap.game_id, "game_id roundtrips");
  require(captured.display_name == snap.display_name, "display_name roundtrips");
  require(captured.portable == snap.portable, "portable roundtrips");
  require(captured.deploy_strategy == snap.deploy_strategy,
          "deploy_strategy roundtrips");
  require(captured.proton_runner == snap.proton_runner, "proton_runner roundtrips");
  require(captured.steam_appid == snap.steam_appid, "steam_appid roundtrips");

  require(captured.mod_entries.size() == snap.mod_entries.size(),
          "mod_entries count roundtrips");
  require(captured.mod_entries["SKSE"].hidden == false,
          "SKSE hidden defaults on capture (not in mod_state)");
  require(captured.mod_entries["ENB"].deploy_at_root == true,
          "ENB deploy_at_root roundtrips");
  require(captured.mod_entries["ENB"].hidden_files.size() == 1,
          "ENB hidden_files roundtrips");
  require(captured.mod_entries["Graphics"].separator_color == "#aabbcc",
          "separator color roundtrips");

  require(captured.profiles.size() == 1, "profiles count roundtrips");
  require(captured.profiles[0].name == "Default", "profile name roundtrips");
  require(captured.profiles[0].mods.size() == 2, "mod count roundtrips");
  require(captured.profiles[0].plugins.size() == 1, "plugins roundtrips");
  require(captured.profiles[0].load_order.size() == 1, "load_order roundtrips");
  require(captured.profiles[0].locked_order.size() == 1, "locked_order roundtrips");
  require(captured.profiles[0].archives.size() == 1, "archives roundtrips");
  require(captured.profiles[0].local_saves == true, "local_saves roundtrips");
  require(captured.profiles[0].auto_archive_invalidation == true,
          "auto_archive_invalidation roundtrips");
  require(captured.profiles[0].tweaked_ini == "[Display]\niSize W=1920\n",
          "tweaked_ini roundtrips");

  fs::remove_all(root);
}

// ---------------------------------------------------------------------------
// apply with no profiles (empty snapshot)
// ---------------------------------------------------------------------------

TEST_CASE("snapshot - empty snapshot roundtrips", "[engine]") {
  using engine::Instance;
  using engine::InstanceSnapshot;

  const auto root = test_root("empty");
  fs::remove_all(root);
  fs::create_directories(root);

  InstanceSnapshot snap;
  snap.game_id      = "TestGame";
  snap.display_name = "Empty";

  Instance inst = Instance::from_root(root);
  require(snap.apply(inst), "apply empty snapshot");

  auto captured = InstanceSnapshot::capture(inst);
  require(captured.game_id == "TestGame", "game_id captured");
  require(captured.display_name == "Empty", "display_name captured");
  require(captured.profiles.empty(), "no profiles");
  require(captured.mod_entries.empty(), "no mod entries");
  require(captured.executables.empty(), "no executables");

  fs::remove_all(root);
}

// ---------------------------------------------------------------------------
// multiple profiles captured and applied
// ---------------------------------------------------------------------------

TEST_CASE("snapshot - multiple profiles", "[engine]") {
  using engine::Instance;
  using engine::InstanceSnapshot;

  const auto root = test_root("multi_profile");
  fs::remove_all(root);
  fs::create_directories(root);

  // Create two profile directories.
  auto p1 = root / "profiles" / "Default";
  auto p2 = root / "profiles" / "Permadeath";
  fs::create_directories(p1);
  fs::create_directories(p2);

  write_text(p1 / "modlist.txt", "+ModA\r\n+ModB\r\n");
  write_text(p1 / "settings.ini", "LocalSaves=true\n");
  write_text(p2 / "modlist.txt", "+ModA\r\n-ModB\r\n");
  write_text(p2 / "settings.ini", "LocalSaves=false\nLocalSettings=true\n");
  write_text(p2 / "plugins.txt", "Dawnguard.esm\n");

  Instance inst = Instance::from_root(root);
  auto snap     = InstanceSnapshot::capture(inst);

  require(snap.profiles.size() == 2, "two profiles captured");

  // Apply to a clean directory.
  const auto root2 = test_root("multi_profile_apply");
  fs::remove_all(root2);
  fs::create_directories(root2);

  Instance inst2 = Instance::from_root(root2);
  require(snap.apply(inst2), "apply succeeds");

  // Verify both profiles written.
  require(fs::exists(root2 / "profiles" / "Default" / "modlist.txt"),
          "Default modlist written");
  require(fs::exists(root2 / "profiles" / "Permadeath" / "modlist.txt"),
          "Permadeath modlist written");
  require(fs::exists(root2 / "profiles" / "Permadeath" / "plugins.txt"),
          "Permadeath plugins written");

  // Capture back and verify.
  auto captured = InstanceSnapshot::capture(inst2);
  require(captured.profiles.size() == 2, "two profiles re-captured");

  fs::remove_all(root);
  fs::remove_all(root2);
}

// ---------------------------------------------------------------------------
// executables round-trip
// ---------------------------------------------------------------------------

TEST_CASE("snapshot - executables round-trip through TOML", "[engine]") {
  using engine::Instance;
  using engine::InstanceSnapshot;

  const auto root = test_root("executables");
  fs::remove_all(root);
  fs::create_directories(root);

  // Write instance.toml with executables.
  write_text(root / "instance.toml", "game_id = \"TestGame\"\n"
                                     "name = \"Exec Test\"\n"
                                     "portable = true\n"
                                     "\n"
                                     "[[executables]]\n"
                                     "path = \"game.exe\"\n"
                                     "title = \"Game\"\n"
                                     "args = \"--debug\"\n"
                                     "cwd = \"/opt/game\"\n"
                                     "mod = \"\"\n"
                                     "icon = \"game.ico\"\n"
                                     "env = [\"LD_LIBRARY_PATH=/opt/lib\"]\n"
                                     "\n"
                                     "[[executables]]\n"
                                     "path = \"editor.exe\"\n"
                                     "title = \"Editor\"\n"
                                     "args = \"\"\n"
                                     "cwd = \"\"\n"
                                     "mod = \"\"\n"
                                     "icon = \"\"\n");

  Instance inst = Instance::from_root(root);
  auto snap     = InstanceSnapshot::capture(inst);

  require(snap.executables.size() == 2, "two executables captured");
  require(snap.executables[0].path == "game.exe", "exec1 path");
  require(snap.executables[0].title == "Game", "exec1 title");
  require(snap.executables[0].args == "--debug", "exec1 args");
  require(snap.executables[0].cwd == "/opt/game", "exec1 cwd");
  require(snap.executables[0].icon == "game.ico", "exec1 icon");
  require(snap.executables[0].env.size() == 1, "exec1 env size");
  require(snap.executables[0].env[0] == "LD_LIBRARY_PATH=/opt/lib", "exec1 env");
  require(snap.executables[1].path == "editor.exe", "exec2 path");

  // Apply to a clean dir.
  const auto root2 = test_root("executables_apply");
  fs::remove_all(root2);
  fs::create_directories(root2);
  Instance inst2 = Instance::from_root(root2);
  require(snap.apply(inst2), "apply succeeds");

  // Verify instance.toml has executables.
  auto toml_str = read_text(root2 / "instance.toml");
  require(toml_str.find("[[executables]]") != std::string::npos, "executables in TOML");
  require(toml_str.find("game.exe") != std::string::npos, "game.exe in TOML");
  require(toml_str.find("editor.exe") != std::string::npos, "editor.exe in TOML");

  // Re-capture.
  auto captured = InstanceSnapshot::capture(inst2);
  require(captured.executables.size() == 2, "executables re-captured");
  require(captured.executables[0].path == "game.exe", "exec1 path re-captured");

  fs::remove_all(root);
  fs::remove_all(root2);
}

// ---------------------------------------------------------------------------
// JSON serialization preserves empty fields correctly
// ---------------------------------------------------------------------------

TEST_CASE("snapshot - JSON omits empty optional fields", "[engine]") {
  using engine::InstanceSnapshot;

  InstanceSnapshot snap;
  snap.game_id      = "TestGame";
  snap.display_name = "Test";

  auto j = snap.to_json();
  require(j.value("game_id", "") == "TestGame", "game_id in JSON");
  require(j.value("display_name", "") == "Test", "display_name in JSON");
  require(j.contains("deploy_strategy") == false, "empty deploy_strategy omitted");
  require(j.contains("proton_runner") == false, "empty proton_runner omitted");
  require(j.contains("steam_appid") == false, "zero steam_appid omitted");
  require(j["mod_entries"].empty(), "empty mod_entries in JSON");
  require(j["profiles"].empty(), "empty profiles in JSON");
  require(j["executables"].empty(), "empty executables in JSON");

  // Reconstruct and verify defaults.
  auto restored = InstanceSnapshot::from_json(j);
  require(restored.game_id == "TestGame", "game_id restored");
  require(restored.deploy_strategy.empty(), "deploy_strategy default");
  require(restored.proton_runner.empty(), "proton_runner default");
  require(restored.steam_appid == 0, "steam_appid default");
}

// ---------------------------------------------------------------------------
// malformed JSON from_json returns empty snapshot
// ---------------------------------------------------------------------------

TEST_CASE("snapshot - from_json handles malformed data", "[engine]") {
  using engine::InstanceSnapshot;

  // Missing required fields - should still work (defaults).
  nlohmann::json j;
  j["game_id"] = "TestGame";
  auto snap    = InstanceSnapshot::from_json(j);
  require(snap.game_id == "TestGame", "minimal valid JSON");
  require(snap.display_name.empty(), "default display_name");

  // Completely empty object.
  auto snap2 = InstanceSnapshot::from_json({});
  require(snap2.game_id.empty(), "empty JSON gives empty snapshot");
}

// ---------------------------------------------------------------------------
// deployed file manifest capture/apply round-trip
// ---------------------------------------------------------------------------

TEST_CASE("snapshot - deployed file manifest round-trip", "[engine]") {
  using engine::Instance;
  using engine::InstanceSnapshot;

  const auto root = test_root("deployed_manifest");
  fs::remove_all(root);
  fs::create_directories(root);

  write_text(root / "instance.toml",
             "game_id = \"TestGame\"\nname = \"Manifest Test\"\n");
  // Ledger TSV: target<TAB>source per line.
  write_text(root / ".gmm_deploy_ledger", "Data/ModA/esp1.esp\t/mods/ModA/esp1.esp\n"
                                          "Data/ModB/mesh.nif\t/mods/ModB/mesh.nif\n");

  Instance inst = Instance::from_root(root);
  auto snap     = InstanceSnapshot::capture(inst);

  require(snap.deployed_files.size() == 2, "two manifest entries captured");
  require(snap.deployed_files[0].target == "Data/ModA/esp1.esp",
          "manifest target captured");
  require(snap.deployed_files[0].source == "/mods/ModA/esp1.esp",
          "manifest source captured");

  // JSON round-trip preserves the manifest.
  auto restored = InstanceSnapshot::from_json(snap.to_json());
  require(restored.deployed_files.size() == 2, "manifest survives JSON");

  // Apply to a clean dir writes the ledger file.
  const auto root2 = test_root("deployed_manifest_apply");
  fs::remove_all(root2);
  fs::create_directories(root2);
  Instance inst2 = Instance::from_root(root2);
  require(snap.apply(inst2), "apply succeeds");
  require(fs::exists(root2 / ".gmm_deploy_ledger"), "ledger written");

  auto captured = InstanceSnapshot::capture(inst2);
  require(captured.deployed_files.size() == 2, "manifest re-captured");

  // Applying an empty-manifest snapshot removes a stale ledger.
  InstanceSnapshot empty_snap;
  empty_snap.game_id      = "TestGame";
  empty_snap.display_name = "Empty";
  require(empty_snap.apply(inst2), "empty apply succeeds");
  require(!fs::exists(root2 / ".gmm_deploy_ledger"), "stale ledger removed");
  require(InstanceSnapshot::capture(inst2).deployed_files.empty(),
          "manifest empty after clear");

  fs::remove_all(root);
  fs::remove_all(root2);
}

// ---------------------------------------------------------------------------
// apply preserves machine-local instance.toml keys it does not own
// ---------------------------------------------------------------------------

TEST_CASE("snapshot - apply preserves unowned toml keys", "[engine]") {
  using engine::Instance;
  using engine::InstanceSnapshot;

  const auto root = test_root("toml_preserve");
  fs::remove_all(root);
  fs::create_directories(root);

  // Existing instance with machine-local keys.
  write_text(root / "instance.toml", "game_id = \"TestGame\"\n"
                                     "name = \"Old Name\"\n"
                                     "game_dir = \"/home/user/games/test\"\n"
                                     "last_tab = \"plugins\"\n");

  InstanceSnapshot snap;
  snap.game_id         = "TestGame";
  snap.display_name    = "New Name";
  snap.deploy_strategy = "symlink";

  Instance inst = Instance::from_root(root);
  require(snap.apply(inst), "apply succeeds");

  auto toml_str = read_text(root / "instance.toml");
  require(toml_str.find("New Name") != std::string::npos, "display name updated");
  require(toml_str.find("/home/user/games/test") != std::string::npos,
          "game_dir preserved");
  require(toml_str.find("last_tab") != std::string::npos, "last_tab preserved");

  fs::remove_all(root);
}

// ---------------------------------------------------------------------------
// modpack identity: stable instance UUID + revision counter (Workspace-5o15)
// ---------------------------------------------------------------------------

namespace {
bool is_uuid_v4(const std::string &s) {
  if (s.size() != 36)
    return false;
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (i == 8 || i == 13 || i == 18 || i == 23)
      if (c != '-')
        return false;
      else
        continue;
    bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    if (!hex)
      return false;
  }
  if (s[14] != '4')
    return false;  // version 4
  if (s[19] != '8' && s[19] != '9' && s[19] != 'a' && s[19] != 'b')
    return false;  // RFC 4122 variant
  return true;
}
}  // namespace

TEST_CASE("snapshot - modpack identity round-trip", "[engine]") {
  using engine::Instance;
  using engine::InstanceSnapshot;

  // UUID helper: valid v4 shape, fresh value per call.
  const std::string id_a = engine::generate_uuid_v4();
  const std::string id_b = engine::generate_uuid_v4();
  require(is_uuid_v4(id_a), "generated id is UUID v4");
  require(is_uuid_v4(id_b), "second id is UUID v4");
  require(id_a != id_b, "generated ids are unique");

  // JSON round-trip preserves pack identity.
  InstanceSnapshot snap;
  snap.game_id          = "TestGame";
  snap.display_name     = "Pack Test";
  snap.modpack_id       = id_a;
  snap.modpack_revision = 3;
  auto restored         = InstanceSnapshot::from_json(snap.to_json());
  require(restored.modpack_id == id_a, "modpack_id survives JSON");
  require(restored.modpack_revision == 3, "modpack_revision survives JSON");

  // Empty identity is omitted from JSON (like deploy_strategy).
  InstanceSnapshot bare;
  bare.game_id   = "TestGame";
  auto bare_json = bare.to_json();
  require(bare_json.contains("modpack_id") == false, "empty modpack_id omitted");
  require(bare_json.contains("modpack_revision") == false,
          "zero modpack_revision omitted");
  auto bare_restored = InstanceSnapshot::from_json(bare_json);
  require(bare_restored.modpack_id.empty(), "modpack_id defaults empty");
  require(bare_restored.modpack_revision == 0, "modpack_revision defaults zero");

  // instance.toml persists both fields (write_toml/read_toml).
  const auto root = test_root("modpack_identity");
  fs::remove_all(root);
  fs::create_directories(root);
  {
    Instance inst                = Instance::from_root(root);
    inst.info().game_id          = "TestGame";
    inst.info().display_name     = "Pack Test";
    inst.info().modpack_id       = id_a;
    inst.info().modpack_revision = 3;
    require(inst.write_toml(), "write_toml with pack identity");
  }
  {
    Instance inst = Instance::from_root(root);
    require(inst.read_toml(), "read_toml succeeds");
    require(inst.info().modpack_id == id_a, "modpack_id persisted");
    require(inst.info().modpack_revision == 3, "modpack_revision persisted");
  }

  // capture() carries the identity; apply() writes it back.
  Instance inst = Instance::from_root(root);
  auto captured = InstanceSnapshot::capture(inst);
  require(captured.modpack_id == id_a, "capture carries modpack_id");
  require(captured.modpack_revision == 3, "capture carries revision");

  const auto root2 = test_root("modpack_identity_apply");
  fs::remove_all(root2);
  fs::create_directories(root2);
  Instance inst2 = Instance::from_root(root2);
  require(captured.apply(inst2), "apply succeeds");
  auto recaptured = InstanceSnapshot::capture(inst2);
  require(recaptured.modpack_id == id_a, "modpack_id survives apply");
  require(recaptured.modpack_revision == 3, "revision survives apply");

  fs::remove_all(root);
  fs::remove_all(root2);
}
