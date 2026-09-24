#pragma once

// Incremental update diff for .gmmpack packs (Workspace-ncdk).
//
// Diffs a new pack revision against the installed-pack record and produces an
// UpdatePlan for the install widget. Pure function over (new pack, state,
// current INI text): never touches disk, network, or Qt.
//
// Per-mod rules (gmmpack-format-v1.md, update step 2):
// - id not tracked in state -> fresh install (installed, conforming)
// - id tracked but absent from the new revision -> removal, but only when
//   presence is still installed (user-removed mods are already gone)
// - origin manual, or presence removed -> untouched, always
// - pinned source fields (file id / version / hash) differ from what the
//   state recorded at apply time -> re-resolve + reinstall
// - updatePolicy latest -> reinstall (re-check upstream; there is no pin)
// - patch set changed for the mod -> reinstall
// - placement diverged never blocks a version/source update (tree step only)
// - otherwise -> skip, zero network calls
//
// Only PRESENT pins are compared: a missing pin means "nothing declared",
// not "changed". A provider switch that keeps identical pins is treated as
// unchanged (exact pins make that a pack-author error, not a real case).
// Name/phase/category/installer-choice-only changes do not trigger a
// reinstall; install order is re-derived by the widget on every update.
//
// Tree diff (step 3): only mods that are untracked (fresh) or tracked as
// installed + conforming + non-manual follow the new tree.json. Diverged
// mods are left exactly where the user put them, including orphaned ones
// whose separator no longer exists. Enabled-flag flips are ignored: only
// position/grouping is pack-owned.
//
// INI diff (step 5): per (targetFile, tweakId). New tweak -> apply, gone
// tweak -> retract by tweak id, same id with different content hash ->
// re-merge + apply. Tweaks owned by a presence-removed mod are retracted.
// The instance toggle wins over the author default: a disabled tweak is
// never applied by an update. Tweaks whose content is unchanged but whose
// on-disk values drifted are flagged (needs current_ini_text), never
// overwritten here; the apply engine flags conflicts at execution time.
//
// Patch diff (step 6): a new/changed/removed patch triggers re-resolution
// of that mod's files (unless the mod is skipped/removed). New or changed
// payloads need fresh consent, scoped to those mods.
//
// Key formats owned by this module (the install widget records them):
// - appliedPatches entries: patch_key() -> "<mod>[#seq]:<hash8>" where the
//   hash covers target path + base sha + algorithm + payload.
// - appliedIniEdits entries: ini_key() -> "<lower target>::<tweak>:<hash8>"
//   where the hash covers the tweak content. Keys without a hash suffix
//   still parse (treated as unknown content, never a reapply).
// - treeSnapshot: raw tree.json text, re-parsed here with parse_tree().

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/core/instance/installed_pack_state.h"
#include "engine/gmmpack/types.h"

namespace engine::modpack {

// "<mod>" or "<mod>#<seq>", stable across revisions for the same patch file.
[[nodiscard]] std::string patch_base(const gmmpack::PatchEntry &patch);
// Full appliedPatches key: base + content hash (see header docs).
[[nodiscard]] std::string patch_key(const gmmpack::PatchEntry &patch);

// Full appliedIniEdits key: lower(target) + tweak id + content hash.
[[nodiscard]] std::string ini_key(const std::string &target_file,
                                  const std::string &tweak_id,
                                  const std::string &content);

// One mod that must be re-resolved + reinstalled, with the human-readable
// reason shown in the widget ("version 1.4.2 -> 1.5", "patch changed", ...).
struct ModChange {
  std::string mod_id;
  std::string reason;
};

struct TreeChange {
  enum class Action { Insert, Move };
  Action action = Action::Insert;
  std::string mod_id;
  std::string new_parent;  // separator path, "" = top level
  std::string old_parent;  // "" for Insert
};

struct IniChange {
  enum class Action { Apply, Retract, Reapply, FlagUserModified };
  Action action = Action::Apply;
  std::string target_file;  // new pack casing
  std::string tweak_id;
  std::string source_mod_id;  // "" = pack-author tweak (or unknown)
  // FlagUserModified only: the drifted key and both values.
  std::string section;
  std::string key;
  std::string expected;  // value the tweak wants
  std::string actual;    // value found on disk
};

struct PatchChange {
  enum class Action { New, Changed, Removed };
  Action action = Action::New;
  std::string mod_id;
  std::string patch_key;  // new key (New/Changed) or applied key (Removed)
};

// The plan handed to the install widget. Empty + an Error diagnostic means
// "not an update" (no pack installed, id mismatch, revision not newer).
struct UpdatePlan {
  std::string pack_id;
  int64_t old_revision = 0;
  int64_t new_revision = 0;
  std::vector<std::string> fresh_installs;  // pack order
  std::vector<ModChange> reinstalls;        // pack order
  std::vector<std::string> removals;        // sorted
  std::vector<TreeChange> tree_changes;
  std::vector<IniChange> ini_changes;
  std::vector<PatchChange> patch_changes;
  std::vector<std::string> patch_consent_mods;  // sorted, newly-patched mods
  std::vector<gmmpack::Diagnostic> diagnostics;
};

// Diffs new_pack (already unpacked + validated) against the installed state.
// current_ini_text maps INI target files (any casing) to their current
// on-disk text; it only enables user-modified flagging and may be empty.
[[nodiscard]] UpdatePlan
diff_update(const gmmpack::Gmmpack &new_pack, const InstalledPackState &state,
            const std::unordered_map<std::string, std::string> &current_ini_text = {});

}  // namespace engine::modpack
