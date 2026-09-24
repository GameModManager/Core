#pragma once

// Append install path (Workspace-pe40) - add a pack's mods to an existing instance.
//
// The wizard routes here through instance_router when the pack's game matches
// the active instance. State rules (gmmpack-format-v1.md):
//   - manual mods: untouched, always (InstalledPackState::skip_in_update),
//   - pack mods with presence removed: never installed,
//   - pack mods with placement diverged: installed, but keep the user's tree
//     position (no tree change emitted),
//   - new pack mods: installed and inserted per tree.json,
//   - lastAppliedRevision recorded for every newly installed mod; tracked
//     entries are never touched (presence/placement preserved).
//
// Reuses (no duplication):
//   - Install::plan_append() as the game-match gate,
//   - Install::detect/resolve/apply_resolutions for user-facing conflicts,
//   - InstalledPackState::skip_in_update()/follows_pack_tree(),
//   - modpack::TreeChange/ini_key()/patch_key() for widget-ready state keys.
//
// Engine layer - Qt-free. Pure planning plus in-memory state seeding: no
// disk, network, or Qt touched (save()/load() stays with the caller).

#include <cstdint>
#include <string>
#include <vector>

#include "engine/core/instance/installed_pack_state.h"
#include "engine/gmmpack/types.h"
#include "engine/install/conflict_resolver.h"
#include "engine/modpack/incremental_update.h"

namespace engine::Install {

// What an append install will do. Lists are disclosure-first: skipped,
// diverged, and unresolved mods are reported, not silently dropped.
struct AppendInstallPlan {
  bool ok = false;
  std::string error;                         // set when !ok
  std::vector<Conflict> conflicts;           // detected before resolution
  std::vector<Resolution> resolutions;       // effective, Ask already defaulted
  InstallPlan mods_to_install;               // post-resolution, state-filtered
  std::vector<std::string> skipped_mods;     // manual or user-removed, sorted
  std::vector<std::string> kept_diverged;    // installed, position kept, sorted
  std::vector<std::string> unresolved_mods;  // pack mods with no resolution, sorted
  std::vector<modpack::TreeChange> tree_changes;  // Insert only, tree.json order
  std::vector<std::string> ini_retract_keys;      // stale applied keys, sorted
  std::vector<std::string> ini_apply_keys;        // new keys, pack order
  std::vector<std::string> patch_consent_mods;    // newly-patched mods, sorted
};

// Builds the append plan. existing_mods are the installed mods as the
// adapter resolved them; pack_resolved are this pack's resolve_mod() outputs
// (entry_id == pack mod id); choices carry the conflict dialog decisions.
// state may be empty (instance never had a pack) - untracked ids install.
[[nodiscard]] AppendInstallPlan
plan_append_install(const gmmpack::Gmmpack &pack, const std::string &instance_game_id,
                    const InstalledPackState &state,
                    const std::vector<Pack::ResolvedMod> &existing_mods,
                    const std::vector<Pack::ResolvedMod> &pack_resolved,
                    const std::vector<UserChoice> &choices);

// Seeds state for newly installed mods: pack identity (only when the
// instance has none) plus one conforming/installed/pack entry per new mod
// with lastAppliedRevision set. Tracked entries are never touched.
void apply_append_state(const AppendInstallPlan &plan, const gmmpack::Gmmpack &pack,
                        InstalledPackState &state);

}  // namespace engine::Install
