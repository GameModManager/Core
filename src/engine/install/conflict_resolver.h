#pragma once

// Append-install conflict resolver - detects what in an incoming pack
// collides with an already-installed instance and turns per-conflict user
// decisions into a modified install plan.
//
// Both sides speak the pack adapter's value types (engine::Pack): installed
// mods are recorded as the ResolvedMod the installer produced for them, pack
// mods are the adapter's resolve_mod() outputs. No new identity types.
//
// Identity keys reuse the existing conflict-detection pattern: file identity
// is vfs::PathResolver::normalize (the same single source ConflictEngine's
// Phase-2 registry uses), so dual-case spellings (Meshes/ + meshes/) match.
//
// Engine layer - Qt-free (only <string>, <vector>, <cstddef>).

#include "engine/pack/adapter.h"

#include <cstddef>
#include <string>
#include <vector>

namespace engine::Install
{

// ---------------------------------------------------------------------------
// Conflicts
// ---------------------------------------------------------------------------

enum class ConflictType
{
  DuplicateSource,  // same provider + mod id (source_type + source_id)
  DuplicateFile,    // different source, same staged archive name
  NameCollision,    // different mod, same display name (diagnostic only)
};

struct Conflict
{
  ConflictType type = ConflictType::DuplicateSource;
  Pack::ResolvedMod existing;  // already installed
  Pack::ResolvedMod pack;      // incoming from the pack
};

// One (existing, pack) pair reports at most one Conflict: the strongest
// match wins (source > file > name). Empty identities never match.
[[nodiscard]] std::vector<Conflict>
detect_conflicts(const std::vector<Pack::ResolvedMod>& existing_mods,
                 const std::vector<Pack::ResolvedMod>& pack_mods);

// ---------------------------------------------------------------------------
// Resolutions
// ---------------------------------------------------------------------------

enum class Action
{
  Skip,     // keep the installed mod, drop the pack entry
  Replace,  // uninstall the installed mod, install the pack entry
  Rename,   // install the pack entry under rename_to
  Keep,     // install as-is (name collisions need no plan change)
  Ask,      // input only: user deferred - resolve_conflicts applies the default
};

struct UserChoice
{
  std::size_t conflict_index = 0;
  Action action              = Action::Ask;
  std::string rename_to;  // only read when action == Rename
};

struct Resolution
{
  std::size_t conflict_index = 0;
  Action action              = Action::Skip;  // effective action, never Ask
  std::string pack_entry_id;                  // conflicts[conflict_index].pack.entry_id
  std::string rename_to;                      // set when action == Rename
  std::string remove_existing;  // existing.entry_id to uninstall (Replace only)
};

// Missing choices and Ask fall back to the safe default per type:
// DuplicateSource/Skip, DuplicateFile/Skip, NameCollision/Keep. Rename with
// an empty rename_to falls back to Skip. Out-of-range choices are ignored;
// when several choices name one conflict, the last wins.
[[nodiscard]] std::vector<Resolution>
resolve_conflicts(const std::vector<Conflict>& conflicts,
                  const std::vector<UserChoice>& user_choices);

// ---------------------------------------------------------------------------
// Install plan
// ---------------------------------------------------------------------------

struct PlanEntry
{
  Pack::ResolvedMod mod;
  // Archive name to install as; empty means mod.archive_name.
  std::string target_name;
};

using InstallPlan = std::vector<PlanEntry>;

// Rewrite the plan from effective resolutions: Skip drops the entry, Rename
// rewrites its target_name, Replace/Keep leave it. Entries with no
// resolution pass through; resolutions naming unknown entries are ignored.
// When several resolutions name one entry, Skip beats Replace beats Rename
// beats Keep.
[[nodiscard]] InstallPlan apply_resolutions(const std::vector<Resolution>& resolutions,
                                            const InstallPlan& install_plan);

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

// One-line human summary for the log; every detected conflict is logged by
// detect_conflicts itself.
[[nodiscard]] std::string describe(const Conflict& conflict);
[[nodiscard]] std::string describe(const Resolution& resolution);

}  // namespace engine::Install
