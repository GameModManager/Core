#pragma once

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
using ProfileGameInitFn = std::function<void(const std::filesystem::path&)>;

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
ProfileCreationResult create_fresh_profile(const std::filesystem::path& profiles_dir, const std::string& name,
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
ProfileCreationResult copy_profile(const std::filesystem::path& profiles_dir, const std::string& new_name,
                                   const std::filesystem::path& source_dir);

// Rename an existing profile directory (MO2's Profile::rename). Validates the
// new name, fails when the target already exists, renames the directory, and
// updates the ProfileName key in settings.ini so the copy is
// self-describing. On failure the profile directory is left untouched and
// false is returned with *error set (when provided).
[[nodiscard]] bool rename_profile(const std::filesystem::path& profiles_dir, const std::string& old_name,
                                  const std::string& new_name, std::string* error = nullptr);

// List existing profile names (directories) under profiles_dir, sorted
// lexicographically. A missing profiles_dir yields an empty list.
[[nodiscard]] std::vector<std::string> list_profiles(const std::filesystem::path& profiles_dir);

// Validate a profile name: non-empty, no path separators, not "." / "..",
// no NUL. Returns false with *error set when invalid.
[[nodiscard]] bool is_valid_profile_name(const std::string& name, std::string* error = nullptr);

}  // namespace engine::profile