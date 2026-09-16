// Engine test for the incremental update diff (Workspace-ncdk): per-mod,
// tree, INI, and patch diffing of a new pack revision against the
// installed-pack record. Builds Gmmpack structs directly, no archives.
#include "engine/modpack/incremental_update.h"

#include <atomic>
#include <filesystem>
#include <optional>
#include <string>
#include <unistd.h>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;
using engine::InstalledPackState;
using engine::PackModPlacement;
using engine::PackModPresence;
using engine::modpack::diff_update;
using engine::modpack::IniChange;
using engine::modpack::ini_key;
using engine::modpack::patch_base;
using engine::modpack::patch_key;
using engine::modpack::PatchChange;
using engine::modpack::TreeChange;
using engine::modpack::UpdatePlan;
namespace gmmpack = engine::gmmpack;

namespace {

std::atomic<int> g_counter{0};

fs::path make_temp_dir(const char* tag) {
  const std::string name = "gmm_update_diff_" + std::string(tag) + "_" +
                           std::to_string(getpid()) + "_" +
                           std::to_string(g_counter.fetch_add(1));
  auto dir = fs::temp_directory_path() / name;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

gmmpack::ModEntry nexus_mod(const std::string& id, int64_t file_id,
                            const std::string& version, const std::string& sha,
                            const std::string& policy = "exact") {
  gmmpack::ModEntry mod;
  mod.id = id;
  mod.name = id;
  gmmpack::ModSourceNexus source;
  source.resolution = "nexus";
  source.game_domain = "skyrimspecialedition";
  source.mod_id = 123;
  source.file_id = file_id;
  source.version = version;
  source.sha256 = sha;
  source.update_policy = policy;
  mod.source = source;
  return mod;
}

gmmpack::PatchEntry patch(const std::string& mod_id,
                          const std::string& payload,
                          std::optional<int> seq = std::nullopt) {
  gmmpack::PatchEntry p;
  p.mod_id = mod_id;
  p.sequence = seq;
  p.target_path = "meshes/" + mod_id + ".nif";
  p.base_file_sha256 = "base";
  p.algorithm = "bsdiff";
  p.payload_base64 = payload;
  return p;
}

gmmpack::IniTweak tweak(const std::string& id, const std::string& content,
                        bool enabled = true,
                        const std::string& source = "") {
  gmmpack::IniTweak t;
  t.id = id;
  t.name = id;
  t.status = "recommended";
  t.enabled = enabled;
  t.content = content;
  t.source_mod_id = source;
  t.has_source_mod_id = !source.empty();
  return t;
}

gmmpack::IniEntry ini_file(const std::string& target,
                           std::vector<gmmpack::IniTweak> tweaks) {
  gmmpack::IniEntry entry;
  entry.target_file = target;
  entry.tweaks = std::move(tweaks);
  return entry;
}

std::string tree_json(const std::string& body) {
  return "{\"nodes\":[" + body + "]}";
}

std::string mod_node(const std::string& id) {
  return "{\"type\":\"mod\",\"id\":\"" + id + "\"}";
}

std::string sep_node(const std::string& name, const std::string& children) {
  return "{\"type\":\"separator\",\"name\":\"" + name +
         "\",\"children\":[" + children + "]}";
}

gmmpack::Gmmpack pack_rev(int revision, std::vector<gmmpack::ModEntry> mods = {}) {
  gmmpack::Gmmpack pack;
  pack.manifest.id = "pack-1";
  pack.manifest.revision = revision;
  pack.mods = std::move(mods);
  return pack;
}

// State for pack-1 at revision 4 with skyui applied as file 100 / 1.0 / aaa.
InstalledPackState seeded_state(const fs::path& root) {
  InstalledPackState state(root);
  state.set_pack("pack-1", 4);
  state.ensure_pack_mod("skyui");
  state.record_pack_version("skyui", 100, "1.0", "aaa", 4);
  return state;
}

bool has_error(const UpdatePlan& plan) {
  for (const auto& d : plan.diagnostics) {
    if (d.severity == gmmpack::Diagnostic::Severity::Error) return true;
  }
  return false;
}

bool has_warning(const UpdatePlan& plan, const std::string& path_part) {
  for (const auto& d : plan.diagnostics) {
    if (d.severity == gmmpack::Diagnostic::Severity::Warning &&
        d.path.find(path_part) != std::string::npos) {
      return true;
    }
  }
  return false;
}

}  // namespace

TEST_CASE("update diff - guards", "[engine]") {
  const fs::path root = make_temp_dir("guards");

  // No installed pack: not an update.
  {
    InstalledPackState state(root / "empty");
    const UpdatePlan plan = diff_update(pack_rev(5), state);
    REQUIRE(has_error(plan));
    REQUIRE(plan.fresh_installs.empty());
  }

  // Pack id mismatch.
  {
    InstalledPackState state = seeded_state(root / "mismatch");
    gmmpack::Gmmpack pack = pack_rev(5);
    pack.manifest.id = "pack-2";
    const UpdatePlan plan = diff_update(pack, state);
    REQUIRE(has_error(plan));
  }

  // Revision not newer: equal and older both refuse.
  {
    InstalledPackState state = seeded_state(root / "stale");
    REQUIRE(has_error(diff_update(pack_rev(4), state)));
    REQUIRE(has_error(diff_update(pack_rev(3), state)));
  }

  // Newer revision passes the guards and reports both revisions.
  {
    InstalledPackState state = seeded_state(root / "ok");
    const UpdatePlan plan = diff_update(pack_rev(5), state);
    REQUIRE(!has_error(plan));
    REQUIRE(plan.pack_id == "pack-1");
    REQUIRE(plan.old_revision == 4);
    REQUIRE(plan.new_revision == 5);
  }
}

TEST_CASE("update diff - per-mod fresh install and removals", "[engine]") {
  const fs::path root = make_temp_dir("modset");

  // Untracked id -> fresh install.
  {
    InstalledPackState state = seeded_state(root / "fresh");
    const UpdatePlan plan =
        diff_update(pack_rev(5, {nexus_mod("skyui", 100, "1.0", "aaa"),
                                 nexus_mod("immersive", 7, "2.0", "bbb")}),
                    state);
    REQUIRE(plan.fresh_installs == std::vector<std::string>{"immersive"});
    REQUIRE(plan.reinstalls.empty());
    REQUIRE(plan.removals.empty());
  }

  // Tracked + installed but absent from the new revision -> removal.
  {
    InstalledPackState state = seeded_state(root / "removal");
    const UpdatePlan plan =
        diff_update(pack_rev(5, {nexus_mod("skyui", 100, "1.0", "aaa")}),
                    state);
    // skyui unchanged; nothing else tracked.
    REQUIRE(plan.removals.empty());

    state.ensure_pack_mod("dropped");
    state.record_pack_version("dropped", 9, "1.0", "zzz", 4);
    const UpdatePlan plan2 =
        diff_update(pack_rev(5, {nexus_mod("skyui", 100, "1.0", "aaa")}),
                    state);
    REQUIRE(plan2.removals == std::vector<std::string>{"dropped"});
  }

  // Already user-removed + absent -> nothing. Manual + absent -> nothing.
  {
    InstalledPackState state = seeded_state(root / "gone");
    state.ensure_pack_mod("dropped");
    state.record_pack_version("dropped", 9, "1.0", "zzz", 4);
    state.mark_removed("dropped");
    state.mark_manual("users-mod");
    const UpdatePlan plan =
        diff_update(pack_rev(5, {nexus_mod("skyui", 100, "1.0", "aaa")}),
                    state);
    REQUIRE(plan.removals.empty());
    REQUIRE(plan.fresh_installs.empty());
    REQUIRE(has_warning(plan, "mods/dropped"));
  }
}

TEST_CASE("update diff - per-mod pins", "[engine]") {
  const fs::path root = make_temp_dir("pins");

  // Unchanged exact mod -> skip.
  {
    InstalledPackState state = seeded_state(root / "same");
    const UpdatePlan plan =
        diff_update(pack_rev(5, {nexus_mod("skyui", 100, "1.0", "aaa")}),
                    state);
    REQUIRE(plan.reinstalls.empty());
    REQUIRE(plan.fresh_installs.empty());
  }

  // Version pin changed -> reinstall with a version reason.
  {
    InstalledPackState state = seeded_state(root / "version");
    const UpdatePlan plan =
        diff_update(pack_rev(5, {nexus_mod("skyui", 100, "1.5", "aaa")}),
                    state);
    REQUIRE(plan.reinstalls.size() == 1);
    REQUIRE(plan.reinstalls[0].mod_id == "skyui");
    REQUIRE(plan.reinstalls[0].reason.find("1.0") != std::string::npos);
    REQUIRE(plan.reinstalls[0].reason.find("1.5") != std::string::npos);
  }

  // File id pin changed -> reinstall.
  {
    InstalledPackState state = seeded_state(root / "file");
    const UpdatePlan plan =
        diff_update(pack_rev(5, {nexus_mod("skyui", 101, "1.0", "aaa")}),
                    state);
    REQUIRE(plan.reinstalls.size() == 1);
    REQUIRE(plan.reinstalls[0].reason.find("100") != std::string::npos);
  }

  // Hash pin changed -> reinstall.
  {
    InstalledPackState state = seeded_state(root / "hash");
    const UpdatePlan plan =
        diff_update(pack_rev(5, {nexus_mod("skyui", 100, "1.0", "bbb")}),
                    state);
    REQUIRE(plan.reinstalls.size() == 1);
    REQUIRE(plan.reinstalls[0].reason == "hash changed");
  }

  // Latest policy -> always re-check upstream.
  {
    InstalledPackState state = seeded_state(root / "latest");
    const UpdatePlan plan =
        diff_update(pack_rev(5, {nexus_mod("skyui", 100, "1.0", "aaa",
                                           "latest")}),
                    state);
    REQUIRE(plan.reinstalls.size() == 1);
    REQUIRE(plan.reinstalls[0].reason == "latest re-check");
  }

  // Manual origin with changed pins -> untouched, always.
  {
    InstalledPackState state = seeded_state(root / "manual");
    state.mark_manual("skyui");
    const UpdatePlan plan =
        diff_update(pack_rev(5, {nexus_mod("skyui", 999, "9.9", "zzz")}),
                    state);
    REQUIRE(plan.reinstalls.empty());
    REQUIRE(plan.fresh_installs.empty());
  }

  // User-removed presence with changed pins -> skipped.
  {
    InstalledPackState state = seeded_state(root / "removed");
    state.mark_removed("skyui");
    const UpdatePlan plan =
        diff_update(pack_rev(5, {nexus_mod("skyui", 999, "9.9", "zzz")}),
                    state);
    REQUIRE(plan.reinstalls.empty());
  }

  // Diverged placement never blocks a version update.
  {
    InstalledPackState state = seeded_state(root / "diverged");
    state.mark_diverged("skyui");
    const UpdatePlan plan =
        diff_update(pack_rev(5, {nexus_mod("skyui", 100, "1.5", "aaa")}),
                    state);
    REQUIRE(plan.reinstalls.size() == 1);
    REQUIRE(plan.reinstalls[0].mod_id == "skyui");
    REQUIRE(has_warning(plan, "mods/skyui"));
  }

  // Missing pins are tolerant: no declared version/hash, same file -> skip.
  {
    InstalledPackState state = seeded_state(root / "nopins");
    gmmpack::ModEntry mod = nexus_mod("skyui", 100, "", "");
    auto& source = std::get<gmmpack::ModSourceNexus>(mod.source);
    source.version.reset();
    source.sha256.reset();
    const UpdatePlan plan = diff_update(pack_rev(5, {mod}), state);
    REQUIRE(plan.reinstalls.empty());
  }
}

TEST_CASE("update diff - tree", "[engine]") {
  const fs::path root = make_temp_dir("tree");
  const std::string old_tree =
      tree_json(sep_node("Graphics", mod_node("skyui")));

  // Conforming mod moved to another separator -> Move.
  {
    InstalledPackState state = seeded_state(root / "move");
    state.set_tree_snapshot(old_tree);
    gmmpack::Gmmpack pack =
        pack_rev(5, {nexus_mod("skyui", 100, "1.0", "aaa")});
    // New tree: skyui now under Interface.
    gmmpack::SeparatorNode sep;
    sep.name = "Interface";
    sep.children.push_back(gmmpack::TreeNode{gmmpack::ModNode{"skyui"}});
    pack.tree.nodes.push_back(gmmpack::TreeNode{sep});
    const UpdatePlan plan = diff_update(pack, state);
    REQUIRE(plan.tree_changes.size() == 1);
    REQUIRE(plan.tree_changes[0].action == TreeChange::Action::Move);
    REQUIRE(plan.tree_changes[0].mod_id == "skyui");
    REQUIRE(plan.tree_changes[0].old_parent == "Graphics");
    REQUIRE(plan.tree_changes[0].new_parent == "Interface");
  }

  // Same layout -> no tree changes.
  {
    InstalledPackState state = seeded_state(root / "same");
    state.set_tree_snapshot(old_tree);
    gmmpack::Gmmpack pack =
        pack_rev(5, {nexus_mod("skyui", 100, "1.0", "aaa")});
    gmmpack::SeparatorNode sep;
    sep.name = "Graphics";
    sep.children.push_back(gmmpack::TreeNode{gmmpack::ModNode{"skyui"}});
    pack.tree.nodes.push_back(gmmpack::TreeNode{sep});
    const UpdatePlan plan = diff_update(pack, state);
    REQUIRE(plan.tree_changes.empty());
  }

  // Diverged mod moved in the new tree -> left alone.
  {
    InstalledPackState state = seeded_state(root / "diverged");
    state.set_tree_snapshot(old_tree);
    state.mark_diverged("skyui");
    gmmpack::Gmmpack pack =
        pack_rev(5, {nexus_mod("skyui", 100, "1.0", "aaa")});
    gmmpack::SeparatorNode sep;
    sep.name = "Interface";
    sep.children.push_back(gmmpack::TreeNode{gmmpack::ModNode{"skyui"}});
    pack.tree.nodes.push_back(gmmpack::TreeNode{sep});
    const UpdatePlan plan = diff_update(pack, state);
    REQUIRE(plan.tree_changes.empty());
  }

  // New mod placed in the new tree -> Insert.
  {
    InstalledPackState state = seeded_state(root / "insert");
    state.set_tree_snapshot(old_tree);
    gmmpack::Gmmpack pack = pack_rev(5, {nexus_mod("skyui", 100, "1.0", "aaa"),
                                         nexus_mod("immersive", 7, "2.0",
                                                   "bbb")});
    gmmpack::SeparatorNode graphics;
    graphics.name = "Graphics";
    graphics.children.push_back(gmmpack::TreeNode{gmmpack::ModNode{"skyui"}});
    gmmpack::SeparatorNode gameplay;
    gameplay.name = "Gameplay";
    gameplay.children.push_back(
        gmmpack::TreeNode{gmmpack::ModNode{"immersive"}});
    pack.tree.nodes.push_back(gmmpack::TreeNode{graphics});
    pack.tree.nodes.push_back(gmmpack::TreeNode{gameplay});
    const UpdatePlan plan = diff_update(pack, state);
    REQUIRE(plan.tree_changes.size() == 1);
    REQUIRE(plan.tree_changes[0].action == TreeChange::Action::Insert);
    REQUIRE(plan.tree_changes[0].mod_id == "immersive");
    REQUIRE(plan.tree_changes[0].new_parent == "Gameplay");
  }

  // Corrupt snapshot -> warning, layout untouched.
  {
    InstalledPackState state = seeded_state(root / "corrupt");
    state.set_tree_snapshot("not json{{{");
    gmmpack::Gmmpack pack =
        pack_rev(5, {nexus_mod("skyui", 100, "1.0", "aaa")});
    gmmpack::SeparatorNode sep;
    sep.name = "Elsewhere";
    sep.children.push_back(gmmpack::TreeNode{gmmpack::ModNode{"skyui"}});
    pack.tree.nodes.push_back(gmmpack::TreeNode{sep});
    const UpdatePlan plan = diff_update(pack, state);
    REQUIRE(plan.tree_changes.empty());
    REQUIRE(has_warning(plan, "tree.json"));
  }

  // Missing snapshot -> warning, layout untouched.
  {
    InstalledPackState state = seeded_state(root / "nosnap");
    gmmpack::Gmmpack pack =
        pack_rev(5, {nexus_mod("skyui", 100, "1.0", "aaa")});
    const UpdatePlan plan = diff_update(pack, state);
    REQUIRE(plan.tree_changes.empty());
    REQUIRE(has_warning(plan, "tree.json"));
  }
}

TEST_CASE("update diff - ini tweaks", "[engine]") {
  const fs::path root = make_temp_dir("ini");
  const std::string content_a = "[Display]\nbShadows=1\n";
  const std::string content_b = "[Display]\nbShadows=0\n";

  auto applied_key = [&](const std::string& target, const std::string& id,
                         const std::string& content) {
    return ini_key(target, id, content);
  };

  // New tweak -> Apply.
  {
    InstalledPackState state = seeded_state(root / "apply");
    gmmpack::Gmmpack pack = pack_rev(5);
    pack.ini_edits.push_back(ini_file("Skyrim.ini", {tweak("shadows",
                                                           content_a)}));
    const UpdatePlan plan = diff_update(pack, state);
    REQUIRE(plan.ini_changes.size() == 1);
    REQUIRE(plan.ini_changes[0].action == IniChange::Action::Apply);
    REQUIRE(plan.ini_changes[0].tweak_id == "shadows");
    REQUIRE(plan.ini_changes[0].target_file == "Skyrim.ini");
  }

  // Applied tweak gone from the new revision -> Retract.
  {
    InstalledPackState state = seeded_state(root / "retract");
    state.set_applied_ini_edits({applied_key("Skyrim.ini", "shadows",
                                             content_a)});
    const UpdatePlan plan = diff_update(pack_rev(5), state);
    REQUIRE(plan.ini_changes.size() == 1);
    REQUIRE(plan.ini_changes[0].action == IniChange::Action::Retract);
    REQUIRE(plan.ini_changes[0].tweak_id == "shadows");
  }

  // Same tweak, same content -> nothing.
  {
    InstalledPackState state = seeded_state(root / "same");
    state.set_applied_ini_edits({applied_key("Skyrim.ini", "shadows",
                                             content_a)});
    gmmpack::Gmmpack pack = pack_rev(5);
    pack.ini_edits.push_back(ini_file("Skyrim.ini", {tweak("shadows",
                                                           content_a)}));
    const UpdatePlan plan = diff_update(pack, state);
    REQUIRE(plan.ini_changes.empty());
  }

  // Same tweak id, changed content -> Reapply.
  {
    InstalledPackState state = seeded_state(root / "reapply");
    state.set_applied_ini_edits({applied_key("Skyrim.ini", "shadows",
                                             content_a)});
    gmmpack::Gmmpack pack = pack_rev(5);
    pack.ini_edits.push_back(ini_file("Skyrim.ini", {tweak("shadows",
                                                           content_b)}));
    const UpdatePlan plan = diff_update(pack, state);
    REQUIRE(plan.ini_changes.size() == 1);
    REQUIRE(plan.ini_changes[0].action == IniChange::Action::Reapply);
  }

  // Tweak owned by a user-removed mod -> Retract even when still shipped.
  {
    InstalledPackState state = seeded_state(root / "owner");
    state.mark_removed("skyui");
    state.set_applied_ini_edits({applied_key("Skyrim.ini", "shadows",
                                             content_a)});
    gmmpack::Gmmpack pack = pack_rev(5);
    pack.ini_edits.push_back(
        ini_file("Skyrim.ini", {tweak("shadows", content_a, true, "skyui")}));
    const UpdatePlan plan = diff_update(pack, state);
    REQUIRE(plan.ini_changes.size() == 1);
    REQUIRE(plan.ini_changes[0].action == IniChange::Action::Retract);
  }

  // Author-disabled new tweak -> skipped with a diagnostic.
  {
    InstalledPackState state = seeded_state(root / "disabled");
    gmmpack::Gmmpack pack = pack_rev(5);
    pack.ini_edits.push_back(
        ini_file("Skyrim.ini", {tweak("shadows", content_a, false)}));
    const UpdatePlan plan = diff_update(pack, state);
    REQUIRE(plan.ini_changes.empty());
    REQUIRE(has_warning(plan, "ini/"));
  }

  // Instance toggle wins: user disabled beats author enabled.
  {
    InstalledPackState state = seeded_state(root / "useroff");
    state.set_ini_tweak("shadows", false);
    gmmpack::Gmmpack pack = pack_rev(5);
    pack.ini_edits.push_back(ini_file("Skyrim.ini", {tweak("shadows",
                                                           content_a, true)}));
    REQUIRE(diff_update(pack, state).ini_changes.empty());
  }

  // Instance toggle wins: user enabled beats author disabled.
  {
    InstalledPackState state = seeded_state(root / "useron");
    state.set_ini_tweak("shadows", true);
    gmmpack::Gmmpack pack = pack_rev(5);
    pack.ini_edits.push_back(
        ini_file("Skyrim.ini", {tweak("shadows", content_a, false)}));
    const UpdatePlan plan = diff_update(pack, state);
    REQUIRE(plan.ini_changes.size() == 1);
    REQUIRE(plan.ini_changes[0].action == IniChange::Action::Apply);
  }

  // Target matching is case-insensitive.
  {
    InstalledPackState state = seeded_state(root / "case");
    state.set_applied_ini_edits({applied_key("skyrim.ini", "shadows",
                                             content_a)});
    gmmpack::Gmmpack pack = pack_rev(5);
    pack.ini_edits.push_back(ini_file("SKYRIM.INI", {tweak("shadows",
                                                           content_a)}));
    REQUIRE(diff_update(pack, state).ini_changes.empty());
  }

  // Drifted on-disk value on an unchanged tweak -> flagged, never applied.
  {
    InstalledPackState state = seeded_state(root / "drift");
    state.set_applied_ini_edits({applied_key("Skyrim.ini", "shadows",
                                             content_a)});
    gmmpack::Gmmpack pack = pack_rev(5);
    pack.ini_edits.push_back(ini_file("Skyrim.ini", {tweak("shadows",
                                                           content_a)}));
    const UpdatePlan plan = diff_update(
        pack, state, {{"skyrim.ini", "[Display]\nbShadows=0\n"}});
    REQUIRE(plan.ini_changes.size() == 1);
    const IniChange& change = plan.ini_changes[0];
    REQUIRE(change.action == IniChange::Action::FlagUserModified);
    REQUIRE(change.section == "Display");
    REQUIRE(change.key == "bShadows");
    REQUIRE(change.expected == "1");
    REQUIRE(change.actual == "0");
  }

  // Matching disk values -> no flags. No disk text -> no flags either.
  {
    InstalledPackState state = seeded_state(root / "nodrift");
    state.set_applied_ini_edits({applied_key("Skyrim.ini", "shadows",
                                             content_a)});
    gmmpack::Gmmpack pack = pack_rev(5);
    pack.ini_edits.push_back(ini_file("Skyrim.ini", {tweak("shadows",
                                                           content_a)}));
    REQUIRE(diff_update(pack, state, {{"Skyrim.ini", content_a}})
                .ini_changes.empty());
    REQUIRE(diff_update(pack, state).ini_changes.empty());
  }
}

TEST_CASE("update diff - patches", "[engine]") {
  const fs::path root = make_temp_dir("patches");

  // New patch -> New + consent + reinstall.
  {
    InstalledPackState state = seeded_state(root / "new");
    gmmpack::Gmmpack pack =
        pack_rev(5, {nexus_mod("skyui", 100, "1.0", "aaa")});
    pack.patches.push_back(patch("skyui", "aaaa"));
    const UpdatePlan plan = diff_update(pack, state);
    REQUIRE(plan.patch_changes.size() == 1);
    REQUIRE(plan.patch_changes[0].action == PatchChange::Action::New);
    REQUIRE(plan.patch_changes[0].mod_id == "skyui");
    REQUIRE(plan.patch_consent_mods == std::vector<std::string>{"skyui"});
    REQUIRE(plan.reinstalls.size() == 1);
    REQUIRE(plan.reinstalls[0].mod_id == "skyui");
    REQUIRE(plan.reinstalls[0].reason == "patch changed");
  }

  // Same patch applied before -> nothing.
  {
    InstalledPackState state = seeded_state(root / "same");
    const gmmpack::PatchEntry entry = patch("skyui", "aaaa");
    state.set_applied_patches({patch_key(entry)});
    gmmpack::Gmmpack pack =
        pack_rev(5, {nexus_mod("skyui", 100, "1.0", "aaa")});
    pack.patches.push_back(entry);
    const UpdatePlan plan = diff_update(pack, state);
    REQUIRE(plan.patch_changes.empty());
    REQUIRE(plan.patch_consent_mods.empty());
    REQUIRE(plan.reinstalls.empty());
  }

  // Same patch file, changed payload -> Changed + consent + reinstall.
  {
    InstalledPackState state = seeded_state(root / "changed");
    state.set_applied_patches({patch_key(patch("skyui", "aaaa"))});
    gmmpack::Gmmpack pack =
        pack_rev(5, {nexus_mod("skyui", 100, "1.0", "aaa")});
    pack.patches.push_back(patch("skyui", "bbbb"));
    const UpdatePlan plan = diff_update(pack, state);
    REQUIRE(plan.patch_changes.size() == 1);
    REQUIRE(plan.patch_changes[0].action == PatchChange::Action::Changed);
    REQUIRE(plan.patch_consent_mods == std::vector<std::string>{"skyui"});
    REQUIRE(plan.reinstalls.size() == 1);
  }

  // Applied patch gone from the new revision -> Removed + reinstall.
  {
    InstalledPackState state = seeded_state(root / "removed");
    const gmmpack::PatchEntry entry = patch("skyui", "aaaa");
    state.set_applied_patches({patch_key(entry)});
    const UpdatePlan plan =
        diff_update(pack_rev(5, {nexus_mod("skyui", 100, "1.0", "aaa")}),
                    state);
    REQUIRE(plan.patch_changes.size() == 1);
    REQUIRE(plan.patch_changes[0].action == PatchChange::Action::Removed);
    REQUIRE(plan.patch_consent_mods.empty());
    REQUIRE(plan.reinstalls.size() == 1);
  }

  // Patch for a user-removed mod -> fully skipped.
  {
    InstalledPackState state = seeded_state(root / "skipped");
    state.mark_removed("skyui");
    state.set_applied_patches({patch_key(patch("skyui", "aaaa"))});
    gmmpack::Gmmpack pack =
        pack_rev(5, {nexus_mod("skyui", 100, "1.0", "aaa")});
    pack.patches.push_back(patch("skyui", "bbbb"));
    const UpdatePlan plan = diff_update(pack, state);
    REQUIRE(plan.patch_changes.empty());
    REQUIRE(plan.reinstalls.empty());
  }

  // Applied patch for a mod dropped from the pack -> removals cover it.
  {
    InstalledPackState state = seeded_state(root / "dropped");
    state.ensure_pack_mod("dropped");
    state.record_pack_version("dropped", 9, "1.0", "zzz", 4);
    state.set_applied_patches({patch_key(patch("dropped", "aaaa"))});
    const UpdatePlan plan =
        diff_update(pack_rev(5, {nexus_mod("skyui", 100, "1.0", "aaa")}),
                    state);
    REQUIRE(plan.patch_changes.empty());
    REQUIRE(plan.removals == std::vector<std::string>{"dropped"});
  }
}

TEST_CASE("update diff - key helpers", "[engine]") {
  const gmmpack::PatchEntry plain = patch("skyui", "aaaa");
  REQUIRE(patch_base(plain) == "skyui");
  const gmmpack::PatchEntry sequenced = patch("skyui", "aaaa", 2);
  REQUIRE(patch_base(sequenced) == "skyui#2");
  REQUIRE(patch_key(plain) != patch_key(sequenced));
  REQUIRE(patch_key(plain) == patch_key(patch("skyui", "aaaa")));

  REQUIRE(ini_key("Skyrim.ini", "shadows", "x") ==
          ini_key("SKYRIM.INI", "shadows", "x"));
  REQUIRE(ini_key("Skyrim.ini", "shadows", "x") !=
          ini_key("Skyrim.ini", "shadows", "y"));
}
