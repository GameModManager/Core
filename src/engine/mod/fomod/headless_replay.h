#pragma once

// Source-agnostic headless FOMOD choice replay.
//
// Consumes InstallerChoices from a collection manifest (the flat
// stepName/groupName -> [pluginNames] map) and replays them onto a
// FomodViewModel without any UI interaction. This is the engine-side
// counterpart of FomodWizardDialog: it drives the same view-model state
// machine from data instead of user clicks.
//
// Engine layer - Qt-free.

#include "engine/collection/manifest.h"
#include "engine/mod/fomod/fomod_view_model.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace engine {

// Diagnostic result of a headless replay.
struct ReplayResult {
    bool success = true;

    // Selection keys (from InstallerChoices::selections) that did not match
    // any stepName/groupName pair in the FOMOD's ModuleConfig.xml.
    std::vector<std::string> unrecognized_keys;

    // Plugin names within a matched group that were not found in the FOMOD XML.
    std::vector<std::string> unrecognized_options;
};

// Replay source-agnostic FOMOD choices onto a FomodViewModel.
//
// For each entry in `choices.selections`, the key is parsed as
// "stepName/groupName" (slash-separated) and matched against the FOMOD
// wizard's install steps and their optional file groups by name. Plugins
// listed in the value vector are toggled ON; plugins not listed are left
// at their default state (the view model's own defaults/required handling
// takes care of the rest).
//
// Returns a ReplayResult with diagnostics for any mismatches. The view
// model is mutated in place: after this call, the selected-plugin state
// reflects the manifest's recorded choices.
[[nodiscard]] ReplayResult replay_fomod_choices(
    FomodViewModel& view_model,
    const Collection::InstallerChoices& choices);

// Resolution of manifest-level choice groups ("pick one of these mods")
// against headless picks, without UI.
//
// `picks` maps ChoiceGroup id -> selected member mod ids. Groups with no
// entry count as "nothing selected". Unknown group ids and non-member mod
// ids are reported as diagnostics and ignored. Mode violations (ExactlyOne
// with zero or 2+ valid picks, AtMostOne with 2+) set success=false and are
// listed in mode_violations, keeping the first valid pick so a headless
// install still proceeds deterministically (warn, don't abort).
struct ChoiceResolution {
    bool success = true;

    // Flattened winning mod ids, in choice-group order.
    std::vector<std::string> selected_mod_ids;

    // Pick keys matching no ChoiceGroup id.
    std::vector<std::string> unrecognized_groups;

    // Selected ids that are not members of their group.
    std::vector<std::string> unrecognized_members;

    // Group ids whose ChoiceMode was violated by the valid picks.
    std::vector<std::string> mode_violations;
};

[[nodiscard]] ChoiceResolution resolve_choice_groups(
    const std::vector<Collection::ChoiceGroup>& groups,
    const std::unordered_map<std::string, std::vector<std::string>>& picks);

} // namespace engine
