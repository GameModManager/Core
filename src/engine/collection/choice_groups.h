#pragma once

// Generic choice-group validation and prior-choice reconciliation.
//
// Choice groups model "pick one of these competing mods" (e.g. texture
// packs) independent of any installer engine: the schema lives in
// Collection::ChoiceGroup (see manifest.h), this module validates picks
// against it and carries prior picks across incremental updates.
//
// Two consumers:
//   - the install UI radio-button step (needs pure per-group validation
//     without mutating the picks), and
//   - the headless/batch path (resolve_choice_groups() in
//     mod/fomod/headless_replay.h keeps first-pick-wins warn-don't-abort
//     semantics for unattended runs).
//
// Engine layer - Qt-free.

#include "engine/collection/manifest.h"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::Collection {

// Group id -> selected member mod ids. Groups with no entry count as
// "nothing selected".
using ChoicePicks = std::unordered_map<std::string, std::vector<std::string>>;

// Per-group validation outcome.
enum class ChoiceStatus
{
  Valid,   // mode satisfied (exactly-one: 1 pick, at-most-one: 0-1 picks)
  Empty,   // exactly-one group with zero valid picks
  TooMany, // 2+ valid picks in either mode
};

struct GroupVerdict
{
  std::string group_id;
  ChoiceStatus status = ChoiceStatus::Valid;
  std::size_t valid_count = 0;
  std::string winning_pick; // first valid pick, empty when none
};

struct ChoiceValidation
{
  bool success = true;
  std::vector<GroupVerdict> verdicts; // one per group, in group order
  std::vector<std::string> unrecognized_groups; // pick keys matching no group
  std::vector<std::string> unrecognized_members; // picks outside their group
};

// Pure per-group validation of picks against choice groups. Unknown group
// ids and non-member picks are reported as diagnostics and excluded from
// the per-group counts; they do not fail validation on their own.
[[nodiscard]] ChoiceValidation validate_choice_groups(
    const std::vector<ChoiceGroup>& groups, const ChoicePicks& picks);

// Picks remembered from a previous install, plus the group membership
// snapshot they were chosen against. Persisted in instance state so an
// incremental update can tell "same options" from "options changed".
struct PriorChoiceState
{
  ChoicePicks picks;
  ChoicePicks membership; // group id -> member mod ids at pick time
};

// Reconcile remembered picks with a new manifest revision for incremental
// updates. An explicit current pick always wins; otherwise the prior pick
// is carried forward only when the group still exists with identical
// membership. Changed/removed groups and unverifiable snapshots (no
// recorded membership) are dropped so the install UI re-prompts instead
// of installing a stale choice.
[[nodiscard]] ChoicePicks reconcile_prior_choices(
    const std::vector<ChoiceGroup>& groups, const PriorChoiceState& prior,
    const ChoicePicks& current_picks);

} // namespace engine::Collection
