#pragma once

// Fresh install path (Workspace-grz6) - install a pack into a brand-new instance.
//
// The instance itself is created by the wizard (instance_router's
// CreateNewInstance route); this module plans what goes into it and seeds
// the installed-pack record. Clean slate: every pack mod installs as
// conforming/installed/pack origin, so there is no divergence to preserve.
//
// Reuses (no duplication):
//   - Install::validate_game_match() for the game gate,
//   - modpack::ini_key()/patch_key() for state-compatible edit/patch keys,
//   - InstalledPackState for the per-mod record.
//
// Engine layer - Qt-free. Pure planning plus in-memory state seeding: no
// disk, network, or Qt touched (save()/load() stays with the caller).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "engine/core/instance/installed_pack_state.h"
#include "engine/gmmpack/types.h"

namespace engine::Install
{

// What a fresh install will do, in widget step order.
struct FreshInstallPlan
{
  bool ok = false;
  std::string error;  // set when !ok
  std::string pack_id;
  std::int64_t revision = 0;
  std::vector<std::string> mods;  // mod ids, pack order
  // Mods carrying patches - the widget gates these behind one consent dialog.
  std::vector<std::string> patch_consent_mods;  // sorted
  // modpack::ini_key() per tweak, pack order - what apply will record.
  std::vector<std::string> ini_keys;
  std::size_t tree_mod_count = 0;  // mod leaves in tree.json
  std::size_t executable_count = 0;
};

// Gates on the game match, then translates pack contents into install steps.
[[nodiscard]] FreshInstallPlan plan_fresh_install(const gmmpack::Gmmpack& pack,
                                                  const std::string& instance_game_id);

// Seeds state for a fresh instance: pack identity plus one
// conforming/installed/pack entry per mod with lastAppliedRevision set.
// Entries are new by construction, so presence/placement defaults hold.
void apply_fresh_state(const gmmpack::Gmmpack& pack, InstalledPackState& state);

}  // namespace engine::Install
