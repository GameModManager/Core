#include "engine/install/fresh_install.h"

#include <algorithm>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include "engine/install/game_match_validator.h"
#include "engine/modpack/incremental_update.h"

namespace engine::Install
{

namespace
{

// Mod leaves under a tree node, depth-first. Separators group only.
[[nodiscard]] std::size_t count_tree_mods(const gmmpack::TreeRoot& tree)
{
  std::size_t count = 0;
  std::vector<const gmmpack::TreeNode*> stack;
  for (const auto& node : tree.nodes)
    stack.push_back(&node);
  while (!stack.empty()) {
    const gmmpack::TreeNode* node = stack.back();
    stack.pop_back();
    if (std::holds_alternative<gmmpack::ModNode>(node->data)) {
      ++count;
    } else {
      for (const auto& child : std::get<gmmpack::SeparatorNode>(node->data).children)
        stack.push_back(&child);
    }
  }
  return count;
}

}  // namespace

FreshInstallPlan plan_fresh_install(const gmmpack::Gmmpack& pack,
                                    const std::string& instance_game_id)
{
  GameMatchResult match =
      validate_game_match(pack.manifest.info.gmm_game_id, instance_game_id);
  if (!match.matches)
    return {false, match.error};

  FreshInstallPlan plan;
  plan.ok       = true;
  plan.pack_id  = pack.manifest.id;
  plan.revision = pack.manifest.revision;

  for (const auto& mod : pack.mods)
    plan.mods.push_back(mod.id);

  std::set<std::string> consent;
  for (const auto& patch : pack.patches)
    consent.insert(patch.mod_id);
  plan.patch_consent_mods.assign(consent.begin(), consent.end());

  for (const auto& ini : pack.ini_edits)
    for (const auto& tweak : ini.tweaks)
      plan.ini_keys.push_back(
          modpack::ini_key(ini.target_file, tweak.id, tweak.content));

  plan.tree_mod_count   = count_tree_mods(pack.tree);
  plan.executable_count = pack.executables.size();
  return plan;
}

void apply_fresh_state(const gmmpack::Gmmpack& pack, InstalledPackState& state)
{
  state.set_pack(pack.manifest.id, pack.manifest.revision);
  for (const auto& mod : pack.mods) {
    state.ensure_pack_mod(mod.id);
    // Fresh entries: nothing resolved yet, so actuals stay empty.
    state.record_pack_version(mod.id, 0, {}, {}, pack.manifest.revision);
  }
}

}  // namespace engine::Install
