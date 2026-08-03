#pragma once

#include <filesystem>

namespace engine {

// Hidden-file markers. GMM hides a mod file by renaming it to <name>.gmmhidden;
// .mohidden is MO2's marker and is recognized so instances shared with MO2 hide
// the same files. Both suffixes are skipped by deployment and shown as hidden
// in the Data tab.
inline constexpr const char* kGmmHiddenSuffix = ".gmmhidden";
inline constexpr const char* kMo2HiddenSuffix = ".mohidden";

// True if the file is hidden by either marker suffix (.gmmhidden or .mohidden).
[[nodiscard]] bool is_hidden_file(const std::filesystem::path& path);

// Hide a file by renaming it to <name>.gmmhidden. No-op if already hidden.
// Returns true on success, false on failure (file kept intact).
bool hide_file(const std::filesystem::path& path);

// Un-hide a file by stripping whichever marker suffix it carries (.gmmhidden
// or .mohidden, restoring the original MO2-compatible name). No-op if not
// hidden. Returns true on success, false on failure.
bool unhide_file(const std::filesystem::path& path);

// Sanitize a directory name to be valid on disk (MO2's fixDirectoryName
// equivalent): Windows-invalid characters (`: < > " ? * | /`, plus `\` on
// Windows) and non-printable/control characters become '_'; leading dots and
// trailing dots/spaces are stripped; reserved Windows device names
// (con, prn, aux, nul, com1-9, lpt1-9) are prefixed with '_'.
// Returns the sanitized name, or empty if nothing usable remains.
[[nodiscard]] std::string sanitize_directory_name(std::string name);

// Remove a file or directory from disk.
//
// By default the path is moved to the platform trash bin (recoverable).
// Pass permanent=true to delete it irreversibly - a "permanently delete"
// user setting will drive this later. Returns true if the path was removed
// (or did not exist), false on failure.
//
// This is the single canonical removal function: every mod / separator /
// download removal in the app routes through it.
bool remove_path(const std::filesystem::path& path, bool permanent = false);

// Move a file or directory from source to dest.
//
// Uses a rename; on a cross-device error (EXDEV) falls back to a recursive
// copy followed by removal of the source, so moves work across filesystems.
// dest is overwritten if it already exists (rename semantics on POSIX); the
// caller is responsible for resolving a name conflict first if a prompt is
// wanted. Returns true on success, false on failure (source kept intact on
// failure).
bool move_path(const std::filesystem::path& source,
               const std::filesystem::path& dest);

// Relay a per-session captured output dir into a mod folder.
//
// scratch_dir holds game-root-relative files captured during a single
// "Output to mod" launch. Files that map into the mod are moved into mod_dir
// (Data-relative layout - the mods_subpath / mod_id prefix is stripped);
// everything else is moved into overwrite_dir as leftover. The scratch dir is
// emptied of remaining directories afterwards (it is NOT removed itself - the
// caller owns its lifetime).
//
// Mapping rules (mirror the game plugin's on-disk layout):
//  - Skyrim-style (include_mod_id=false): scratch/<mods_subpath>/<rest>
//    maps to mod_dir/<rest>  (mods_subpath is e.g. "Data").
//  - Isaac-style (include_mod_id=true): scratch/<mods_subpath>/<mod_id>/<rest>
//    maps to mod_dir/<rest>  (mod_id is the mod's folder name).
// Files not under mods_subpath (or under a different mod's folder) go to
// overwrite_dir keeping their game-root-relative path.
//
// Returns the number of files relayed into mod_dir.
size_t relay_output_to_mod(const std::filesystem::path& scratch_dir,
                           const std::filesystem::path& mod_dir,
                           const std::filesystem::path& overwrite_dir,
                           const std::string& mods_subpath,
                           bool include_mod_id,
                           const std::string& mod_id);

}  // namespace engine
