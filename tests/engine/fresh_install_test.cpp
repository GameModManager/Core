// Engine test for the fresh install path (Workspace-grz6): game gating,
// plan contents (mods, patch consent, INI keys, tree/executable counts),
// and clean-slate state seeding. Builds Gmmpack structs directly.
#include "engine/install/fresh_install.h"

#include <filesystem>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace gmmpack = engine::gmmpack;
using engine::InstalledPackState;
using engine::PackModOrigin;
using engine::PackModPlacement;
using engine::PackModPresence;
using engine::Install::apply_fresh_state;
using engine::Install::FreshInstallPlan;
using engine::Install::plan_fresh_install;

namespace {

gmmpack::Gmmpack make_pack() {
  gmmpack::Gmmpack pack;
  pack.manifest.id               = "pack-1";
  pack.manifest.revision         = 4;
  pack.manifest.info.gmm_game_id = "skyrimspecialedition";
  pack.manifest.info.name        = "Pack One";

  gmmpack::ModEntry alpha;
  alpha.id   = "alpha";
  alpha.name = "Alpha";
  gmmpack::ModEntry beta;
  beta.id   = "beta";
  beta.name = "Beta";
  pack.mods = {alpha, beta};

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

  gmmpack::ExecutableEntry exe;
  exe.id            = "loot";
  exe.relative_path = "LOOT.exe";
  exe.role          = "setup";
  pack.executables  = {exe};
  return pack;
}

}  // namespace

TEST_CASE("fresh install rejects game mismatch", "[fresh_install]") {
  const gmmpack::Gmmpack pack = make_pack();
  FreshInstallPlan plan       = plan_fresh_install(pack, "fallout4");
  REQUIRE_FALSE(plan.ok);
  REQUIRE_FALSE(plan.error.empty());
  REQUIRE(plan.error.find("skyrimspecialedition") != std::string::npos);
  REQUIRE(plan.error.find("fallout4") != std::string::npos);
  REQUIRE(plan.mods.empty());
}

TEST_CASE("fresh install rejects undeclared game", "[fresh_install]") {
  gmmpack::Gmmpack pack = make_pack();
  pack.manifest.info.gmm_game_id.clear();
  FreshInstallPlan plan = plan_fresh_install(pack, "fallout4");
  REQUIRE_FALSE(plan.ok);
  REQUIRE_FALSE(plan.error.empty());
}

TEST_CASE("fresh install plans every pack mod in order", "[fresh_install]") {
  const gmmpack::Gmmpack pack = make_pack();
  FreshInstallPlan plan       = plan_fresh_install(pack, "SkyrimSE");
  REQUIRE(plan.ok);
  REQUIRE(plan.error.empty());
  REQUIRE(plan.pack_id == "pack-1");
  REQUIRE(plan.revision == 4);
  REQUIRE(plan.mods == std::vector<std::string>{"alpha", "beta"});
  REQUIRE(plan.patch_consent_mods == std::vector<std::string>{"beta"});
  REQUIRE(plan.ini_keys.size() == 1);
  REQUIRE(plan.ini_keys.front().find("shadows") != std::string::npos);
  REQUIRE(plan.tree_mod_count == 2);
  REQUIRE(plan.executable_count == 1);
}

TEST_CASE("fresh install matches through game aliases", "[fresh_install]") {
  const gmmpack::Gmmpack pack = make_pack();
  FreshInstallPlan plan       = plan_fresh_install(pack, "skyrimse");
  REQUIRE(plan.ok);
  REQUIRE(plan.mods.size() == 2);
}

TEST_CASE("fresh install handles empty pack", "[fresh_install]") {
  gmmpack::Gmmpack pack;
  pack.manifest.id               = "empty";
  pack.manifest.info.gmm_game_id = "fallout4";
  FreshInstallPlan plan          = plan_fresh_install(pack, "fallout4");
  REQUIRE(plan.ok);
  REQUIRE(plan.mods.empty());
  REQUIRE(plan.patch_consent_mods.empty());
  REQUIRE(plan.ini_keys.empty());
  REQUIRE(plan.tree_mod_count == 0);
  REQUIRE(plan.executable_count == 0);
}

TEST_CASE("fresh install consent list is sorted and deduplicated", "[fresh_install]") {
  gmmpack::Gmmpack pack = make_pack();
  gmmpack::PatchEntry second;
  second.mod_id           = "alpha";
  second.target_path      = "meshes/alpha.nif";
  second.base_file_sha256 = "base";
  second.algorithm        = "bsdiff";
  second.payload_base64   = "bbb";
  pack.patches.push_back(second);
  pack.patches.push_back(second);  // duplicate patch file, one consent
  FreshInstallPlan plan = plan_fresh_install(pack, "skyrimspecialedition");
  REQUIRE(plan.ok);
  REQUIRE(plan.patch_consent_mods == std::vector<std::string>{"alpha", "beta"});
}

TEST_CASE("fresh install seeds conforming state with revision", "[fresh_install]") {
  const gmmpack::Gmmpack pack = make_pack();
  InstalledPackState state(std::filesystem::temp_directory_path() / "gmm_fresh_state");
  apply_fresh_state(pack, state);

  REQUIRE(state.has_pack());
  REQUIRE(state.pack_id() == "pack-1");
  REQUIRE(state.installed_revision() == 4);
  for (const std::string &id : {"alpha", "beta"}) {
    const auto *entry = state.resolved_mod(id);
    REQUIRE(entry != nullptr);
    CHECK(entry->origin == PackModOrigin::Pack);
    CHECK(entry->presence == PackModPresence::Installed);
    CHECK(entry->placement == PackModPlacement::Conforming);
    CHECK(entry->last_applied_revision == 4);
    CHECK(entry->actual_file_id == 0);
    CHECK(entry->actual_version.empty());
  }
}
