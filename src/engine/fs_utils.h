#pragma once

#include <filesystem>

namespace engine {

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
