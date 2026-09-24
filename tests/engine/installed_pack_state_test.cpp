// Engine test for the installed-pack record (Workspace-by7k): per-mod
// origin/presence/placement tracking, pack identity, tree snapshot, applied
// patches/ini edits, ini tweak toggles. JSON round-trip through
// installed_pack.json, atomic writes, missing/corrupt file handling.
// Uses temp dirs only, no Qt.
#include "engine/core/instance/installed_pack_state.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

std::atomic<int> g_counter{0};

fs::path make_temp_dir(const char *tag) {
  const std::string name = "gmm_pack_state_" + std::string(tag) + "_" +
                           std::to_string(getpid()) + "_" +
                           std::to_string(g_counter.fetch_add(1));
  auto dir               = fs::temp_directory_path() / name;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

void write_text(const fs::path &path, const std::string &content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

}  // namespace

TEST_CASE("installed pack state - empty", "[engine]") {
  using engine::InstalledPackState;

  const fs::path root = make_temp_dir("empty");
  InstalledPackState state(root);

  // Missing file is valid empty state.
  REQUIRE(state.load());
  REQUIRE(!state.has_pack());
  REQUIRE(state.pack_id().empty());
  REQUIRE(state.installed_revision() == 0);
  REQUIRE(!state.is_tracked("skyui"));
  REQUIRE(state.resolved_mod("skyui") == nullptr);
  // Untracked ids are not skipped (fresh install) and don't follow the tree.
  REQUIRE(!state.skip_in_update("skyui"));
  REQUIRE(!state.follows_pack_tree("skyui"));
  REQUIRE(state.tree_snapshot().empty());
  REQUIRE(state.applied_patches().empty());
  REQUIRE(state.applied_ini_edits().empty());
  REQUIRE(state.ini_tweaks().empty());
  REQUIRE(!state.ini_tweak_enabled("anisotropy").has_value());
}

TEST_CASE("installed pack state - pack identity roundtrip", "[engine]") {
  using engine::InstalledPackState;

  const fs::path root = make_temp_dir("identity");
  InstalledPackState state(root);
  REQUIRE(state.load());

  state.set_pack("b3f1e2a0-uuid4", 4);
  REQUIRE(state.has_pack());
  REQUIRE(state.pack_id() == "b3f1e2a0-uuid4");
  REQUIRE(state.installed_revision() == 4);
  REQUIRE(state.save());

  InstalledPackState reloaded(root);
  REQUIRE(reloaded.load());
  REQUIRE(reloaded.has_pack());
  REQUIRE(reloaded.pack_id() == "b3f1e2a0-uuid4");
  REQUIRE(reloaded.installed_revision() == 4);

  reloaded.clear();
  REQUIRE(!reloaded.has_pack());
  REQUIRE(!reloaded.is_tracked("skyui"));
  REQUIRE(reloaded.save());

  InstalledPackState cleared(root);
  REQUIRE(cleared.load());
  REQUIRE(!cleared.has_pack());
}

TEST_CASE("installed pack state - resolved mod roundtrip", "[engine]") {
  using engine::InstalledPackState;
  using engine::PackModOrigin;
  using engine::PackModPlacement;
  using engine::PackModPresence;

  const fs::path root = make_temp_dir("resolved");
  InstalledPackState state(root);
  state.set_pack("pack-1", 4);
  state.record_pack_version("skyui", 67890, "1.4.2", "sha256:abc", 4);

  const auto *e = state.resolved_mod("skyui");
  REQUIRE(e != nullptr);
  REQUIRE(e->origin == PackModOrigin::Pack);
  REQUIRE(e->presence == PackModPresence::Installed);
  REQUIRE(e->placement == PackModPlacement::Conforming);
  REQUIRE(e->last_applied_revision == 4);
  REQUIRE(e->actual_file_id == 67890);
  REQUIRE(e->actual_version == "1.4.2");
  REQUIRE(e->actual_hash == "sha256:abc");
  REQUIRE(e->produced_by.empty());
  REQUIRE(state.is_tracked("skyui"));
  REQUIRE(!state.skip_in_update("skyui"));
  REQUIRE(state.follows_pack_tree("skyui"));
  REQUIRE(state.save());

  InstalledPackState reloaded(root);
  REQUIRE(reloaded.load());
  const auto *r = reloaded.resolved_mod("skyui");
  REQUIRE(r != nullptr);
  REQUIRE(r->origin == PackModOrigin::Pack);
  REQUIRE(r->presence == PackModPresence::Installed);
  REQUIRE(r->placement == PackModPlacement::Conforming);
  REQUIRE(r->last_applied_revision == 4);
  REQUIRE(r->actual_file_id == 67890);
  REQUIRE(r->actual_version == "1.4.2");
  REQUIRE(r->actual_hash == "sha256:abc");
}

TEST_CASE("installed pack state - manual origin is outside pack diff", "[engine]") {
  using engine::InstalledPackState;
  using engine::PackModOrigin;

  const fs::path root = make_temp_dir("manual");
  InstalledPackState state(root);
  state.mark_manual("my-tweak-mod");
  REQUIRE(state.resolved_mod("my-tweak-mod")->origin == PackModOrigin::Manual);
  REQUIRE(state.skip_in_update("my-tweak-mod"));
  REQUIRE(!state.follows_pack_tree("my-tweak-mod"));
  REQUIRE(state.save());

  InstalledPackState reloaded(root);
  REQUIRE(reloaded.load());
  REQUIRE(reloaded.resolved_mod("my-tweak-mod")->origin == PackModOrigin::Manual);
  REQUIRE(reloaded.skip_in_update("my-tweak-mod"));
}

TEST_CASE("installed pack state - removed and restored", "[engine]") {
  using engine::InstalledPackState;
  using engine::PackModPresence;

  const fs::path root = make_temp_dir("removed");
  InstalledPackState state(root);
  state.ensure_pack_mod("awesome-mod");
  REQUIRE(!state.skip_in_update("awesome-mod"));

  state.mark_removed("awesome-mod");
  REQUIRE(state.resolved_mod("awesome-mod")->presence == PackModPresence::Removed);
  REQUIRE(state.skip_in_update("awesome-mod"));
  REQUIRE(!state.follows_pack_tree("awesome-mod"));
  REQUIRE(state.save());

  InstalledPackState reloaded(root);
  REQUIRE(reloaded.load());
  REQUIRE(reloaded.skip_in_update("awesome-mod"));

  // Explicit restore brings it back into the update flow.
  reloaded.restore_from_pack("awesome-mod");
  REQUIRE(reloaded.resolved_mod("awesome-mod")->presence == PackModPresence::Installed);
  REQUIRE(!reloaded.skip_in_update("awesome-mod"));
  REQUIRE(reloaded.follows_pack_tree("awesome-mod"));
}

TEST_CASE("installed pack state - diverged keeps versions, skips tree", "[engine]") {
  using engine::InstalledPackState;
  using engine::PackModPlacement;

  const fs::path root = make_temp_dir("diverged");
  InstalledPackState state(root);
  state.record_pack_version("skyui", 67890, "1.4.2", "hash-a", 4);
  state.mark_diverged("skyui");
  REQUIRE(state.resolved_mod("skyui")->placement == PackModPlacement::Diverged);
  // Divergence never blocks version updates, only position changes.
  REQUIRE(!state.skip_in_update("skyui"));
  REQUIRE(!state.follows_pack_tree("skyui"));

  // A later pack revision re-resolves the mod: version fields move,
  // placement stays diverged (no self-heal).
  state.record_pack_version("skyui", 67900, "1.4.3", "hash-b", 5);
  REQUIRE(state.resolved_mod("skyui")->actual_version == "1.4.3");
  REQUIRE(state.resolved_mod("skyui")->actual_file_id == 67900);
  REQUIRE(state.resolved_mod("skyui")->last_applied_revision == 5);
  REQUIRE(state.resolved_mod("skyui")->placement == PackModPlacement::Diverged);
  REQUIRE(state.save());

  InstalledPackState reloaded(root);
  REQUIRE(reloaded.load());
  REQUIRE(reloaded.resolved_mod("skyui")->placement == PackModPlacement::Diverged);
  REQUIRE(!reloaded.skip_in_update("skyui"));
  REQUIRE(!reloaded.follows_pack_tree("skyui"));

  // Only an explicit reset returns the mod to the pack layout.
  reloaded.reset_to_pack_layout("skyui");
  REQUIRE(reloaded.resolved_mod("skyui")->placement == PackModPlacement::Conforming);
  REQUIRE(reloaded.follows_pack_tree("skyui"));
}

TEST_CASE("installed pack state - generated mods", "[engine]") {
  using engine::InstalledPackState;
  using engine::PackModOrigin;

  const fs::path root = make_temp_dir("generated");
  InstalledPackState state(root);
  state.mark_generated("nemesis-output", "nemesis");
  const auto *e = state.resolved_mod("nemesis-output");
  REQUIRE(e->origin == PackModOrigin::Generated);
  REQUIRE(e->produced_by == "nemesis");
  // Generated output still participates in updates and the tree.
  REQUIRE(!state.skip_in_update("nemesis-output"));
  REQUIRE(state.follows_pack_tree("nemesis-output"));
  REQUIRE(state.save());

  InstalledPackState reloaded(root);
  REQUIRE(reloaded.load());
  const auto *r = reloaded.resolved_mod("nemesis-output");
  REQUIRE(r->origin == PackModOrigin::Generated);
  REQUIRE(r->produced_by == "nemesis");
}

TEST_CASE("installed pack state - tree snapshot and applied lists", "[engine]") {
  using engine::InstalledPackState;

  const fs::path root = make_temp_dir("snapshot");
  InstalledPackState state(root);
  state.set_tree_snapshot(R"({"nodes":[{"type":"mod","id":"skyui"}]})");
  state.set_applied_patches({"awesome-mod-1", "awesome-mod-2"});
  state.set_applied_ini_edits({"skyrim:anisotropy"});
  REQUIRE(state.save());

  InstalledPackState reloaded(root);
  REQUIRE(reloaded.load());
  REQUIRE(reloaded.tree_snapshot() == R"({"nodes":[{"type":"mod","id":"skyui"}]})");
  const std::vector<std::string> want_patches = {"awesome-mod-1", "awesome-mod-2"};
  REQUIRE(reloaded.applied_patches() == want_patches);
  const std::vector<std::string> want_edits = {"skyrim:anisotropy"};
  REQUIRE(reloaded.applied_ini_edits() == want_edits);
}

TEST_CASE("installed pack state - ini tweaks", "[engine]") {
  using engine::InstalledPackState;

  const fs::path root = make_temp_dir("tweaks");
  InstalledPackState state(root);
  // Author default seeds fresh installs; thereafter the instance owns it.
  state.set_ini_tweak("anisotropy", true);
  state.set_ini_tweak("skyui-archives", false);
  REQUIRE(state.ini_tweak_enabled("anisotropy") == std::optional<bool>(true));
  REQUIRE(state.ini_tweak_enabled("skyui-archives") == std::optional<bool>(false));
  REQUIRE(!state.ini_tweak_enabled("unknown-tweak").has_value());
  // User toggles off a required tweak; the record owns the toggle now.
  state.set_ini_tweak("anisotropy", false);
  REQUIRE(state.ini_tweak_enabled("anisotropy") == std::optional<bool>(false));
  REQUIRE(state.save());

  InstalledPackState reloaded(root);
  REQUIRE(reloaded.load());
  REQUIRE(reloaded.ini_tweak_enabled("anisotropy") == std::optional<bool>(false));
  REQUIRE(reloaded.ini_tweak_enabled("skyui-archives") == std::optional<bool>(false));
  REQUIRE(reloaded.ini_tweaks().size() == 2);
}

TEST_CASE("installed pack state - corrupt file fails gracefully", "[engine]") {
  using engine::InstalledPackState;

  const fs::path root = make_temp_dir("corrupt");

  // Truncated JSON.
  write_text(root / "installed_pack.json", R"({"packId": "pack-1", "resolvedMods": {)");
  InstalledPackState bad(root);
  REQUIRE(!bad.load());
  REQUIRE(!bad.has_pack());

  // Valid JSON but wrong top-level type.
  write_text(root / "installed_pack.json", R"([1, 2, 3])");
  InstalledPackState wrong_type(root);
  REQUIRE(!wrong_type.load());

  // Wrong field types.
  write_text(root / "installed_pack.json", R"({"packId": 42})");
  InstalledPackState wrong_field(root);
  REQUIRE(!wrong_field.load());

  // A failed load leaves in-memory state untouched.
  InstalledPackState keep(root);
  keep.set_pack("pack-1", 2);
  write_text(root / "installed_pack.json", R"({oops)");
  REQUIRE(!keep.load());
  REQUIRE(keep.pack_id() == "pack-1");
  REQUIRE(keep.installed_revision() == 2);

  // Non-object entries and non-bool tweak values are handled.
  write_text(root / "installed_pack.json",
             R"({"packId":"p","resolvedMods":{"a":42,"b":{"origin":"pack"}},)"
             R"("iniTweaks":{"t":"yes"}})");
  InstalledPackState mixed(root);
  REQUIRE(!mixed.load());  // "yes" is not a bool: whole load fails, state kept

  write_text(root / "installed_pack.json",
             R"({"packId":"p","resolvedMods":{"a":42,"b":{"origin":"pack"}}})");
  InstalledPackState skipped(root);
  REQUIRE(skipped.load());
  REQUIRE(!skipped.is_tracked("a"));
  REQUIRE(skipped.is_tracked("b"));
}

TEST_CASE("installed pack state - enum string mapping", "[engine]") {
  using engine::pack_origin_from_string;
  using engine::pack_placement_from_string;
  using engine::pack_presence_from_string;
  using engine::PackModOrigin;
  using engine::PackModPlacement;
  using engine::PackModPresence;
  using engine::to_string;

  REQUIRE(std::string(to_string(PackModOrigin::Pack)) == "pack");
  REQUIRE(std::string(to_string(PackModOrigin::Manual)) == "manual");
  REQUIRE(std::string(to_string(PackModOrigin::Generated)) == "generated");
  REQUIRE(std::string(to_string(PackModPresence::Installed)) == "installed");
  REQUIRE(std::string(to_string(PackModPresence::Removed)) == "removed");
  REQUIRE(std::string(to_string(PackModPlacement::Conforming)) == "conforming");
  REQUIRE(std::string(to_string(PackModPlacement::Diverged)) == "diverged");

  REQUIRE(pack_origin_from_string("pack") == PackModOrigin::Pack);
  REQUIRE(pack_origin_from_string("manual") == PackModOrigin::Manual);
  REQUIRE(pack_origin_from_string("generated") == PackModOrigin::Generated);
  REQUIRE(pack_presence_from_string("installed") == PackModPresence::Installed);
  REQUIRE(pack_presence_from_string("removed") == PackModPresence::Removed);
  REQUIRE(pack_placement_from_string("conforming") == PackModPlacement::Conforming);
  REQUIRE(pack_placement_from_string("diverged") == PackModPlacement::Diverged);

  // Unknown strings fall back to pack defaults (forward compatibility).
  REQUIRE(pack_origin_from_string("bogus") == PackModOrigin::Pack);
  REQUIRE(pack_presence_from_string("bogus") == PackModPresence::Installed);
  REQUIRE(pack_placement_from_string("bogus") == PackModPlacement::Conforming);
  REQUIRE(pack_origin_from_string("") == PackModOrigin::Pack);
}
