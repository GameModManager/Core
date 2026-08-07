#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace engine {

// Canonical home for overwrite-folder workflows (MO2 `modlistviewactions`
// / `syncoverwritedialog` port). The UI layer is the primary consumer, but
// the CLI / headless launcher share these same primitives - nothing is
// duplicated.
//
// Path-space model (matches GMM's on-disk layout):
//   - Overwrite files are GAME-ROOT-relative (e.g. "Data/meshes/foo.nif",
//     "ControlMap_Custom.txt") because they are captured from the game dir.
//   - Mod folders are flat: the mod folder root IS the game install target
//     root for that game (Skyrim: `mods/SkyUI/SkyUI_SE.bsa` with
//     mods_subpath="Data"; Isaac: `mods/M/resources/gfx` with mods_subpath="").
//   - overwrite_to_mod_rel() is the bridge between the two.

// A mod that provides a file, ordered for the sync dialog.
struct OverwriteOwner {
    std::string mod_id;   // mod folder name
    int priority = 0;     // as recorded by the conflict engine
};

// Per-file sync decision built by collect_overwrite_sync_files().
struct OverwriteSyncFile {
    std::string overwrite_rel;                 // game-root-relative path
    std::vector<OverwriteOwner> owners;        // winner first, then alternatives
    bool game_has_file = false;                // vanilla game copy exists
};

// One moved file in an applied sync plan.
struct OverwriteSyncTarget {
    std::string overwrite_rel;
    std::string mod_folder;                    // destination mod folder ("" = don't sync)
};

// Normalize an overwrite-relative (game-root-relative) path into the
// mod-relative path the file should have inside a mod folder.
//
// Mirrors relay_output_to_mod's mapping rules:
//   - mods_subpath non-empty and path under "<mods_subpath>/" ->
//     strip "<mods_subpath>/" (Skyrim: "Data/meshes/x" -> "meshes/x").
//     The prefix match is case-insensitive: Isaac's game dir writes "Mods/"
//     while the knowledge registry says "mods".
//   - include_mod_id and path under "<mods_subpath>/<mod_id>/" ->
//     strip both (Isaac: "mods/MyMod/resources/x" -> "resources/x").
//   - otherwise the path is passed through unchanged.
std::string overwrite_to_mod_rel(const std::string& overwrite_rel,
                                 const std::string& mods_subpath,
                                 bool include_mod_id = false,
                                 const std::string& mod_id = {});

// Move ALL contents of overwrite_dir into mod_dir, normalizing each path with
// overwrite_to_mod_rel() (so a Skyrim "Data/..." file lands at the mod root).
// Emptied overwrite dirs are pruned. Cross-device fallback (copy + remove).
// Returns false on the first failure.
bool move_overwrite_to_mod(const std::filesystem::path& overwrite_dir,
                           const std::filesystem::path& mod_dir,
                           const std::string& mods_subpath,
                           bool include_mod_id = false,
                           const std::string& mod_id = {});

// Move a single overwrite entry (file or directory, given as an absolute
// path under overwrite_dir) into mod_dir. A mapping-root directory (e.g.
// "Data") has its contents moved (MO2 ModList::dropLocalFiles semantics for
// overwrite-origin drops); anything else moves wholesale. Returns false when
// entry is not under overwrite_dir or the move fails.
bool move_overwrite_entry_to_mod(const std::filesystem::path& overwrite_dir,
                                 const std::filesystem::path& entry_path,
                                 const std::filesystem::path& mod_dir,
                                 const std::string& mods_subpath,
                                 bool include_mod_id = false,
                                 const std::string& mod_id = {});

// Move a single overwrite file into dest_mod_dir, removing any existing
// destination first (MO2 SyncOverwriteDialog::applyTo). Returns false on
// failure. Emptied overwrite dirs are pruned. include_mod_id / mod_id mirror
// overwrite_to_mod_rel() for include_mod_id games (Isaac).
bool sync_overwrite_file(const std::filesystem::path& overwrite_dir,
                         const std::string& overwrite_rel,
                         const std::filesystem::path& dest_mod_dir,
                         const std::string& mods_subpath,
                         bool include_mod_id = false,
                         const std::string& mod_id = {});

// True when overwrite is effectively empty - an empty mod-mapping root dir
// (mods_subpath, e.g. "Data") does not count as content (MO2
// ModInfoOverwrite::isEmpty).
bool overwrite_is_empty(const std::filesystem::path& overwrite_dir,
                        const std::string& mods_subpath = {});

// Clear overwrite. Contents of the mod-mapping root dir (mods_subpath) are
// deleted but the root dir itself is kept; everything else is deleted whole
// (MO2 ModListViewActions::clearOverwrite). All deletions go to the system
// trash via engine::remove_path(). Returns true when the folder is gone/empty.
bool clear_overwrite(const std::filesystem::path& overwrite_dir,
                     const std::string& mods_subpath = {});

// True when the game ships a regular file at the game-root-relative path.
bool game_has_file(const std::filesystem::path& game_dir,
                   const std::string& overwrite_rel);

// Build the per-file sync decisions for "Sync to Mods...".
//
// mod_infos must be (folder_name, priority) for ENABLED managed mods only -
// Overwrite / MERGED / game-native are excluded by the caller. A fresh
// ConflictEngine::compute (all extensions - the sync dialog must see every
// file, unlike the flags column) produces the owners; the winner is the
// highest-priority owner (lowest when conflict_reversed, Isaac convention).
// For include_mod_id games (Isaac) the mod-relative key is the game-root path
// with "<mods_subpath>/<mod-folder>/" stripped. A file with no mod owner but a
// vanilla game copy gets game_has_file=true.
std::vector<OverwriteSyncFile> collect_overwrite_sync_files(
    const std::filesystem::path& overwrite_dir,
    const std::filesystem::path& mods_dir,
    const std::vector<std::pair<std::string, int>>& mod_infos,
    const std::string& mods_subpath,
    bool conflict_reversed,
    bool include_mod_id = false,
    const std::filesystem::path& game_dir = {});

// Apply a sync plan chosen by the dialog: for each target, move the overwrite
// file into mods_dir/<target.mod_folder>/ (creating that mod folder and
// writing its metadata file when it does not yet exist - this is how a
// "game origin" destination becomes a real mod). Returns the number of files
// moved. include_mod_id applies to every target; the mod-id segment stripped
// from the path is the target's own mod_folder (Isaac convention). The dialog
// resolves the game-origin destination to a concrete folder name before
// calling.
size_t apply_sync_plan(const std::vector<OverwriteSyncTarget>& targets,
                       const std::filesystem::path& overwrite_dir,
                       const std::filesystem::path& mods_dir,
                       const std::string& mods_subpath,
                       const std::string& metadata_file,
                       bool include_mod_id = false);

// Merge case-insensitive-duplicate directories in overwrite_dir so the folder
// follows the SAME CI rule the deploy and conflict registry use
// (resolve_deploy_target_ci / normalize_ci_key). Windows games resolve paths
// case-insensitively, but every Linux capture target is case-sensitive - the
// overlay upperdir (Overwrite itself) receives the game's raw writes, so a
// logical "Data/Meshes" splits across "Meshes" and "meshes". Directory
// components merge into one surviving casing (most content wins, byte-lexic
// smallest name breaks ties - title-cased "Meshes" beats "meshes"); CI-equal
// FILE names stay side-by-side (the same rare-packaging-bug exception the
// deploy makes). Idempotent and cheap on an already-normalized tree (one
// directory listing per dir, zero merges). Symlinks are never followed or
// merged. Returns the number of directories merged away.
std::size_t normalize_overwrite_casing(
    const std::filesystem::path& overwrite_dir);

}  // namespace engine
