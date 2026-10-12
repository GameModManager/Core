#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace engine::profile {

// Result of a profile creation attempt. `success` is false and `error` holds
// a human-readable reason when the profile could not be created; on success
// `directory` is the created profile directory.
struct ProfileCreationResult {
  bool success = false;
  std::string error;
  std::filesystem::path directory;
};

// Game-specific initialization callback (MO2's IPluginGame::initializeProfile
// parity). Called with the created profile directory after the base files
// (settings.ini, modlist.txt, archives.txt) exist, so the callback can create
// game-specific files (game INIs, load order, saves). May be empty when no
// game plugin is available.
using ProfileGameInitFn = std::function<void(const std::filesystem::path &)>;

// Create a fresh profile directory under profiles_dir (MO2's
// Profile::Profile(name, gamePlugin, features, useDefaultSettings)).
//
// Steps:
//   1. Validate the name (see is_valid_profile_name) and fail when the
//      profile already exists.
//   2. Create the profile directory (and profiles_dir when missing).
//   3. Create empty modlist.txt and archives.txt.
//   4. Call game_init (when set) so the game plugin can create game-specific
//      files.
//   5. Auto-detect LocalSaves/LocalSettings from the resulting directory
//      state (MO2's findProfileSettings): a saves/ subdir implies
//      LocalSaves=true, a _saves subdir implies false; any game INI file
//      (a *.ini other than settings.ini) in the profile implies
//      LocalSettings=true.
//   6. Write settings.ini with the detected values (defaults when nothing
//      was detected: LocalSaves=false, LocalSettings=false,
//      AutomaticArchiveInvalidation=false).
//
// On failure the partially-created directory is removed and the error is
// returned. The created profile is NOT loaded into a Profile object - callers
// construct one from `directory` when they need to mutate it.
ProfileCreationResult create_fresh_profile(const std::filesystem::path &profiles_dir,
                                           const std::string &name,
                                           ProfileGameInitFn game_init = {});

// Copy an existing profile directory to a new name under profiles_dir (MO2's
// Profile::createPtrFrom(name, reference, gamePlugin)).
//
// - Validates the new name and fails when the target already exists.
// - Recursively copies the source profile directory (modlist.txt,
//   plugins.txt, loadorder.txt, lockedorder.txt, archives.txt, saves/, game
//   INIs - all mod/plugin state is preserved verbatim).
// - Updates settings.ini with the new profile name (root key "ProfileName").
//
// On failure the partially-copied directory is removed and the error is
// returned.
ProfileCreationResult copy_profile(const std::filesystem::path &profiles_dir,
                                   const std::string &new_name,
                                   const std::filesystem::path &source_dir);

// Rename an existing profile directory (MO2's Profile::rename). Validates the
// new name, fails when the target already exists, renames the directory, and
// updates the ProfileName key in settings.ini so the copy is
// self-describing. On failure the profile directory is left untouched and
// false is returned with *error set (when provided).
[[nodiscard]] bool rename_profile(const std::filesystem::path &profiles_dir,
                                  const std::string &old_name,
                                  const std::string &new_name,
                                  std::string *error = nullptr);

// List existing profile names (directories) under profiles_dir, sorted
// lexicographically. A missing profiles_dir yields an empty list.
[[nodiscard]] std::vector<std::string>
list_profiles(const std::filesystem::path &profiles_dir);

// Validate a profile name: non-empty, no path separators, not "." / "..",
// no NUL. Returns false with *error set when invalid.
[[nodiscard]] bool is_valid_profile_name(const std::string &name,
                                         std::string *error = nullptr);

// Outcome of rename_mod_in_all_profiles. `rewritten` counts the profile
// files that took the new name; `failed` names the profiles that list the mod
// but could NOT be updated (the modlist could not be read, or the write was
// refused). A name in `failed` means that profile still refers to the mod by
// its old folder name, so the mod resolves there as missing and loses its load
// order and enabled state - which is the damage this function exists to
// prevent. It is never safe to leave a failure unreported.
struct ProfileModRenameResult {
  std::size_t rewritten = 0;
  std::vector<std::string> failed;
};

// Rewrite `old_name` to `new_name` in every profile's modlist.txt under
// profiles_dir (MO2's Profile::renameModInAllProfiles, profile.cpp:317).
//
// A mod's identity is its folder name, and every profile records that name in
// its own modlist.txt with the mod's priority and enabled state. Renaming the
// folder therefore orphans the entry in every profile but the active one: the
// mod silently reverts to "new mod" defaults in those profiles, losing both
// its load order and its enabled state. Every profile is updated here, so the
// rename stays true everywhere.
//
// There is NO rollback and NO all-or-nothing guarantee, and deliberately so:
// the caller has already renamed the folder by the time this runs, so
// unwinding the modlists alone would orphan the mod in every profile instead
// of some - strictly worse. A profile whose modlist.txt is missing or does not
// mention the mod is skipped, not a failure: that profile does not use the
// mod. A profile that lists the mod but cannot be read or written IS a failure
// and comes back in `failed` for the caller to tell the user about.
ProfileModRenameResult
rename_mod_in_all_profiles(const std::filesystem::path &profiles_dir,
                           const std::string &old_name, const std::string &new_name);

}  // namespace engine::profile