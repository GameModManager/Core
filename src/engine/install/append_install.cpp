#include "engine/install/append_install.h"

#include <algorithm>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "engine/install/instance_router.h"

namespace engine::Install {

namespace {

  using ResolvedById = std::unordered_map<std::string, Pack::ResolvedMod>;

  // Installable pack mods: pack order, minus state-skipped (manual/removed),
  // minus entries the adapter never resolved (reported, not installed).
  void split_candidates(const gmmpack::Gmmpack &pack, const InstalledPackState &state,
                        const ResolvedById &resolved, InstallPlan &candidates,
                        std::vector<std::string> &skipped,
                        std::vector<std::string> &unresolved) {
    for (const auto &mod : pack.mods) {
      if (state.skip_in_update(mod.id)) {
        skipped.push_back(mod.id);
        continue;
      }
      auto it = resolved.find(mod.id);
      if (it == resolved.end()) {
        unresolved.push_back(mod.id);
        continue;
      }
      candidates.push_back({it->second, {}});
    }
  }

  // Walk tree.json depth-first, emitting Insert for every installable mod that
  // still follows the pack layout. Diverged mods keep the user's position.
  void collect_tree_inserts(const gmmpack::TreeNode &node, const std::string &parent,
                            const InstalledPackState &state,
                            const std::unordered_set<std::string> &installable,
                            std::unordered_set<std::string> &seen,
                            std::vector<modpack::TreeChange> &out,
                            std::vector<std::string> &kept_diverged) {
    if (std::holds_alternative<gmmpack::ModNode>(node.data)) {
      const std::string &id = std::get<gmmpack::ModNode>(node.data).id;
      if (!installable.contains(id))
        return;
      seen.insert(id);
      if (state.is_tracked(id) && !state.follows_pack_tree(id)) {
        kept_diverged.push_back(id);
        return;
      }
      out.push_back({modpack::TreeChange::Action::Insert, id, parent, {}});
      return;
    }
    const auto &sep = std::get<gmmpack::SeparatorNode>(node.data);
    const std::string child_parent =
        parent.empty() ? sep.name : parent + "/" + sep.name;
    for (const auto &child : sep.children)
      collect_tree_inserts(child, child_parent, state, installable, seen, out,
                           kept_diverged);
  }

}  // namespace

AppendInstallPlan
plan_append_install(const gmmpack::Gmmpack &pack, const std::string &instance_game_id,
                    const InstalledPackState &state,
                    const std::vector<Pack::ResolvedMod> &existing_mods,
                    const std::vector<Pack::ResolvedMod> &pack_resolved,
                    const std::vector<UserChoice> &choices) {
  // Step 1: the router's game-match gate - appends never cross games.
  AppendPlan gate = plan_append(pack.manifest.info.gmm_game_id, instance_game_id);
  if (!gate.ok)
    return {false, gate.error};

  AppendInstallPlan plan;
  plan.ok = true;

  std::unordered_map<std::string, Pack::ResolvedMod> resolved_by_id;
  for (const auto &mod : pack_resolved)
    resolved_by_id[mod.entry_id] = mod;

  InstallPlan candidates;
  split_candidates(pack, state, resolved_by_id, candidates, plan.skipped_mods,
                   plan.unresolved_mods);

  // Steps 2-4: conflicts against installed mods, resolved by dialog choices.
  std::vector<Pack::ResolvedMod> candidate_mods;
  for (const auto &entry : candidates)
    candidate_mods.push_back(entry.mod);
  plan.conflicts       = detect_conflicts(existing_mods, candidate_mods);
  plan.resolutions     = resolve_conflicts(plan.conflicts, choices);
  plan.mods_to_install = apply_resolutions(plan.resolutions, candidates);

  std::unordered_set<std::string> installable;
  for (const auto &entry : plan.mods_to_install)
    installable.insert(entry.mod.entry_id);

  // Step 6: tree merge - conforming and fresh mods follow tree.json.
  std::unordered_set<std::string> seen_in_tree;
  for (const auto &node : pack.tree.nodes)
    collect_tree_inserts(node, {}, state, installable, seen_in_tree, plan.tree_changes,
                         plan.kept_diverged);
  // Installable mods missing from tree.json land at top level, pack order.
  // (seen covers diverged mods too - they were visited, just not moved.)
  for (const auto &entry : plan.mods_to_install) {
    if (installable.contains(entry.mod.entry_id) &&
        !seen_in_tree.contains(entry.mod.entry_id))
      plan.tree_changes.push_back(
          {modpack::TreeChange::Action::Insert, entry.mod.entry_id, {}, {}});
  }

  // Step 7: INI edits - retract stale applied keys, apply new ones unless
  // the instance toggle owns them off.
  std::vector<std::pair<std::string, std::string>> new_tweaks;  // key, tweak id
  for (const auto &ini : pack.ini_edits)
    for (const auto &tweak : ini.tweaks)
      new_tweaks.emplace_back(
          modpack::ini_key(ini.target_file, tweak.id, tweak.content), tweak.id);
  std::unordered_set<std::string> new_keys;
  for (const auto &[key, _] : new_tweaks)
    new_keys.insert(key);
  std::set<std::string> retract;
  for (const auto &key : state.applied_ini_edits())
    if (!new_keys.contains(key))
      retract.insert(key);
  plan.ini_retract_keys.assign(retract.begin(), retract.end());
  std::unordered_set<std::string> applied(state.applied_ini_edits().begin(),
                                          state.applied_ini_edits().end());
  for (const auto &[key, tweak_id] : new_tweaks) {
    if (applied.contains(key))
      continue;
    if (state.ini_tweak_enabled(tweak_id).value_or(true))
      plan.ini_apply_keys.push_back(key);
  }

  // Step 8: patches - fresh consent scoped to mods whose patch content is new.
  std::unordered_set<std::string> applied_patches(state.applied_patches().begin(),
                                                  state.applied_patches().end());
  std::set<std::string> consent;
  for (const auto &patch : pack.patches) {
    if (!installable.contains(patch.mod_id))
      continue;
    if (!applied_patches.contains(modpack::patch_key(patch)))
      consent.insert(patch.mod_id);
  }
  plan.patch_consent_mods.assign(consent.begin(), consent.end());

  std::sort(plan.skipped_mods.begin(), plan.skipped_mods.end());
  std::sort(plan.unresolved_mods.begin(), plan.unresolved_mods.end());
  std::sort(plan.kept_diverged.begin(), plan.kept_diverged.end());
  return plan;
}

void apply_append_state(const AppendInstallPlan &plan, const gmmpack::Gmmpack &pack,
                        InstalledPackState &state) {
  if (!state.has_pack())
    state.set_pack(pack.manifest.id, pack.manifest.revision);
  for (const auto &entry : plan.mods_to_install) {
    const std::string &id = entry.mod.entry_id;
    if (state.is_tracked(id))
      continue;  // presence/placement stay user-owned, always
    state.ensure_pack_mod(id);
    // Newly installed: nothing resolved yet, so actuals stay empty.
    state.record_pack_version(id, 0, {}, {}, pack.manifest.revision);
  }
}

}  // namespace engine::Install
