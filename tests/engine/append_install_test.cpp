// Engine test for the append install path (Workspace-pe40): game gating,
// state filtering (manual/removed/diverged), conflict resolution plumbing,
// tree merging, INI retract/apply, patch consent, and state seeding.
#include "engine/install/append_install.h"

#include <atomic>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace fs      = std::filesystem;
namespace gmmpack = engine::gmmpack;
using engine::InstalledPackState;
using engine::PackModPlacement;
using engine::Install::AppendInstallPlan;
using engine::Install::apply_append_state;
using engine::Install::plan_append_install;
using engine::Install::UserChoice;
using engine::modpack::TreeChange;
using engine::Pack::ResolvedMod;

namespace {

std::atomic<int> g_counter{0};

fs::path make_temp_dir(const char *tag) {
  const std::string name = "gmm_append_" + std::string(tag) + "_" +
                           std::to_string(getpid()) + "_" +
                           std::to_string(g_counter.fetch_add(1));
  auto dir               = fs::temp_directory_path() / name;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

gmmpack::Gmmpack make_pack() {
  gmmpack::Gmmpack pack;
  pack.manifest.id               = "pack-1";
  pack.manifest.revision         = 7;
  pack.manifest.info.gmm_game_id = "skyrimspecialedition";

  for (const std::string &id : {"alpha", "beta", "gamma"}) {
    gmmpack::ModEntry mod;
    mod.id   = id;
    mod.name = id;
    pack.mods.push_back(std::move(mod));
  }

  gmmpack::PatchEntry patch;
  patch.mod_id           = "beta";
  patch.target_path      = "meshes/beta.nif";
  patch.base_file_sha256 = "base";
  patch.algorithm        = "bsdiff";
  patch.payload_base64   = "aaa";
  pack.patches           = {patch};

  gmmpack::IniTweak tweak;
  tweak.id      = "shadows";
  tweak.name    = "Shadows";
  tweak.status  = "recommended";
  tweak.content = "[Display]\niShadowMapResolution=2048\n";
  gmmpack::IniEntry ini;
  ini.target_file = "Skyrim.ini";
  ini.tweaks      = {tweak};
  pack.ini_edits  = {ini};

  gmmpack::SeparatorNode sep;
  sep.name = "Graphics";
  sep.children.push_back(gmmpack::TreeNode{gmmpack::ModNode{"alpha"}});
  sep.children.push_back(gmmpack::TreeNode{gmmpack::ModNode{"beta"}});
  gmmpack::TreeNode top;
  top.data = std::move(sep);
  pack.tree.nodes.push_back(std::move(top));
  // gamma has no tree node: lands at top level.
  return pack;
}

ResolvedMod resolved(const std::string &id, const std::string &source_id = {},
                     const std::string &archive = {}) {
  ResolvedMod mod;
  mod.entry_id     = id;
  mod.display_name = id + " display";
  mod.archive_name = archive.empty() ? id + ".zip" : archive;
  mod.source_type  = "nexus";
  mod.source_id    = source_id.empty() ? "src-" + id : source_id;
  return mod;
}

std::vector<ResolvedMod> resolve_all() {
  return {resolved("alpha"), resolved("beta"), resolved("gamma")};
}

bool installs(const AppendInstallPlan &plan, const std::string &id) {
  for (const auto &entry : plan.mods_to_install)
    if (entry.mod.entry_id == id)
      return true;
  return false;
}

}  // namespace

TEST_CASE("append install rejects game mismatch", "[append_install]") {
  const gmmpack::Gmmpack pack = make_pack();
  InstalledPackState state(make_temp_dir("mismatch"));
  AppendInstallPlan plan =
      plan_append_install(pack, "fallout4", state, {}, resolve_all(), {});
  REQUIRE_FALSE(plan.ok);
  REQUIRE_FALSE(plan.error.empty());
  REQUIRE(plan.mods_to_install.empty());
}

TEST_CASE("append install plans fresh mods into empty state", "[append_install]") {
  const gmmpack::Gmmpack pack = make_pack();
  InstalledPackState state(make_temp_dir("empty"));
  AppendInstallPlan plan =
      plan_append_install(pack, "skyrimspecialedition", state, {}, resolve_all(), {});
  REQUIRE(plan.ok);
  REQUIRE(plan.error.empty());
  REQUIRE(plan.conflicts.empty());
  REQUIRE(plan.skipped_mods.empty());
  REQUIRE(plan.unresolved_mods.empty());
  REQUIRE(plan.mods_to_install.size() == 3);
  CHECK(plan.mods_to_install[0].mod.entry_id == "alpha");
  // Tree order: alpha + beta under Graphics, gamma untracked by tree.json.
  REQUIRE(plan.tree_changes.size() == 3);
  CHECK(plan.tree_changes[0].mod_id == "alpha");
  CHECK(plan.tree_changes[0].new_parent == "Graphics");
  CHECK(plan.tree_changes[1].mod_id == "beta");
  CHECK(plan.tree_changes[2].mod_id == "gamma");
  CHECK(plan.tree_changes[2].new_parent.empty());
  for (const auto &change : plan.tree_changes)
    CHECK(change.action == TreeChange::Action::Insert);
  REQUIRE(plan.ini_apply_keys.size() == 1);
  REQUIRE(plan.patch_consent_mods == std::vector<std::string>{"beta"});
}

TEST_CASE("append install skips manual and removed mods", "[append_install]") {
  const gmmpack::Gmmpack pack = make_pack();
  InstalledPackState state(make_temp_dir("skip"));
  state.mark_manual("alpha");
  state.ensure_pack_mod("gamma");
  state.mark_removed("gamma");
  AppendInstallPlan plan =
      plan_append_install(pack, "skyrimspecialedition", state, {}, resolve_all(), {});
  REQUIRE(plan.ok);
  REQUIRE(plan.skipped_mods == std::vector<std::string>{"alpha", "gamma"});
  REQUIRE_FALSE(installs(plan, "alpha"));
  REQUIRE_FALSE(installs(plan, "gamma"));
  REQUIRE(installs(plan, "beta"));
  // Skipped mods get no tree changes either.
  for (const auto &change : plan.tree_changes)
    CHECK(change.mod_id == "beta");
}

TEST_CASE("append install keeps diverged positions", "[append_install]") {
  const gmmpack::Gmmpack pack = make_pack();
  InstalledPackState state(make_temp_dir("diverged"));
  state.ensure_pack_mod("alpha");
  state.mark_diverged("alpha");
  AppendInstallPlan plan =
      plan_append_install(pack, "skyrimspecialedition", state, {}, resolve_all(), {});
  REQUIRE(plan.ok);
  // Diverged mods still install (position only is user-owned) ...
  REQUIRE(installs(plan, "alpha"));
  REQUIRE(plan.kept_diverged == std::vector<std::string>{"alpha"});
  // ... but follow no tree change.
  for (const auto &change : plan.tree_changes)
    CHECK(change.mod_id != "alpha");
}

TEST_CASE("append install reports unresolved pack mods", "[append_install]") {
  const gmmpack::Gmmpack pack = make_pack();
  InstalledPackState state(make_temp_dir("unresolved"));
  std::vector<ResolvedMod> partial = {resolved("alpha")};
  AppendInstallPlan plan =
      plan_append_install(pack, "skyrimspecialedition", state, {}, partial, {});
  REQUIRE(plan.ok);
  REQUIRE(plan.unresolved_mods == std::vector<std::string>{"beta", "gamma"});
  REQUIRE(installs(plan, "alpha"));
  REQUIRE_FALSE(installs(plan, "beta"));
}

TEST_CASE("append install skips conflicting mod by default", "[append_install]") {
  const gmmpack::Gmmpack pack = make_pack();
  InstalledPackState state(make_temp_dir("conflict-skip"));
  // Same provider + mod id as the pack's beta: DuplicateSource.
  const std::vector<ResolvedMod> existing = {
      resolved("old-beta", "src-beta", "old.zip")};
  AppendInstallPlan plan = plan_append_install(pack, "skyrimspecialedition", state,
                                               existing, resolve_all(), {});
  REQUIRE(plan.ok);
  REQUIRE(plan.conflicts.size() == 1);
  CHECK(plan.conflicts.front().pack.entry_id == "beta");
  REQUIRE_FALSE(installs(plan, "beta"));
  REQUIRE(installs(plan, "alpha"));
}

TEST_CASE("append install honors replace and rename choices", "[append_install]") {
  const gmmpack::Gmmpack pack             = make_pack();
  const std::vector<ResolvedMod> existing = {
      resolved("old-beta", "src-beta", "old.zip")};

  {
    InstalledPackState state(make_temp_dir("conflict-replace"));
    UserChoice choice;
    choice.conflict_index  = 0;
    choice.action          = engine::Install::Action::Replace;
    AppendInstallPlan plan = plan_append_install(pack, "skyrimspecialedition", state,
                                                 existing, resolve_all(), {choice});
    REQUIRE(plan.ok);
    REQUIRE(installs(plan, "beta"));
  }
  {
    InstalledPackState state(make_temp_dir("conflict-rename"));
    UserChoice choice;
    choice.conflict_index  = 0;
    choice.action          = engine::Install::Action::Rename;
    choice.rename_to       = "beta-pack.zip";
    AppendInstallPlan plan = plan_append_install(pack, "skyrimspecialedition", state,
                                                 existing, resolve_all(), {choice});
    REQUIRE(plan.ok);
    REQUIRE(installs(plan, "beta"));
    for (const auto &entry : plan.mods_to_install)
      if (entry.mod.entry_id == "beta")
        CHECK(entry.target_name == "beta-pack.zip");
  }
}

TEST_CASE("append install retracts stale ini and honors toggles", "[append_install]") {
  const gmmpack::Gmmpack pack = make_pack();
  InstalledPackState state(make_temp_dir("ini"));
  state.set_applied_ini_edits({"skyrim.ini::old-tweak:deadbeef"});
  state.set_ini_tweak("shadows", false);  // instance owns the toggle: off
  AppendInstallPlan plan =
      plan_append_install(pack, "skyrimspecialedition", state, {}, resolve_all(), {});
  REQUIRE(plan.ok);
  REQUIRE(plan.ini_retract_keys ==
          std::vector<std::string>{"skyrim.ini::old-tweak:deadbeef"});
  REQUIRE(plan.ini_apply_keys.empty());
}

TEST_CASE("append install skips consent for applied patches", "[append_install]") {
  const gmmpack::Gmmpack pack = make_pack();
  InstalledPackState state(make_temp_dir("patches"));
  state.set_applied_patches({engine::modpack::patch_key(pack.patches.front())});
  AppendInstallPlan plan =
      plan_append_install(pack, "skyrimspecialedition", state, {}, resolve_all(), {});
  REQUIRE(plan.ok);
  REQUIRE(plan.patch_consent_mods.empty());
}

TEST_CASE("append install seeds new mods, keeps tracked", "[append_install]") {
  const gmmpack::Gmmpack pack = make_pack();
  InstalledPackState state(make_temp_dir("seed"));
  state.set_pack("other-pack", 3);
  state.ensure_pack_mod("alpha");
  state.mark_diverged("alpha");
  AppendInstallPlan plan =
      plan_append_install(pack, "skyrimspecialedition", state, {}, resolve_all(), {});
  REQUIRE(plan.ok);
  apply_append_state(plan, pack, state);

  // Existing pack identity and the diverged mod survive untouched ...
  CHECK(state.pack_id() == "other-pack");
  CHECK(state.installed_revision() == 3);
  const auto *alpha = state.resolved_mod("alpha");
  REQUIRE(alpha != nullptr);
  CHECK(alpha->placement == PackModPlacement::Diverged);
  CHECK(alpha->last_applied_revision == 0);
  // ... while new mods are tracked with the pack revision.
  const auto *beta = state.resolved_mod("beta");
  REQUIRE(beta != nullptr);
  CHECK(beta->placement == PackModPlacement::Conforming);
  CHECK(beta->last_applied_revision == 7);
}

TEST_CASE("append install sets pack identity when none", "[append_install]") {
  const gmmpack::Gmmpack pack = make_pack();
  InstalledPackState state(make_temp_dir("seed-fresh"));
  AppendInstallPlan plan =
      plan_append_install(pack, "skyrimspecialedition", state, {}, resolve_all(), {});
  REQUIRE(plan.ok);
  apply_append_state(plan, pack, state);
  CHECK(state.pack_id() == "pack-1");
  CHECK(state.installed_revision() == 7);
}
