#pragma once

#include "engine/profile/profile.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace engine::PluginDb {
class Database;
}
namespace engine {
using PluginDatabase = PluginDb::Database;  // backward-compat alias
}

namespace engine::profile {

// Result of a profile switch attempt. On success `profile` owns the new
// active Profile (the caller takes ownership and drops its previous one);
// `changed` is false when the requested profile was already active (MO2's
// early return in OrganizerCore::setCurrentProfile) — the caller keeps its
// current Profile and `profile` is null.
struct ProfileSwitchResult {
  bool success = false;
  bool changed = false;
  std::string error;
  std::unique_ptr<ProfileManager> profile;
};

// Live state snapshot of the current profile, gathered by the caller (UI)
// and persisted by save_current_profile() before a switch (MO2's
// saveCurrentProfile). The engine is Qt-free and does not own the mod list /
// archives widgets, so the caller collects the live state and the switcher
// writes it through the Profile's atomic writers.
struct ProfileSaveState {
  // All mod ids present in the instance's mods directory, and the subset
  // that is unmanaged (DLC etc.). Used to converge modlist.txt
  // (refresh_mod_status) before the immediate flush — mods not yet in the
  // file are appended so the on-disk state matches the mods dir.
  std::vector<std::string> known_mods;
  std::vector<std::string> foreign_mods;
  // Enabled archives (archives.txt). Always written (may be empty).
  std::vector<std::string> archives;
  // Tweaked INI content (initweaks.ini). Written only when non-empty
  // (MO2's createTweakedIniFile "if needed").
  std::string tweaked_ini;
};

// UI-owned side effects the Qt-free engine cannot perform itself. The
// switcher drives the transition and invokes these at the right points
// (MO2's setCurrentProfile sequence). Any callback may be empty; empty
// callbacks are skipped.
struct ProfileSwitchCallbacks {
  // Re-scan the mods directory and rebuild the mod list (MO2's
  // refreshDirectoryStructure). Called after the new profile's mod state
  // is restored so the UI's ModList reflects the new profile.
  std::function<void()> refresh_directory_structure;
  // Reload the Plugins tab from the new profile's plugin files (MO2's
  // refreshLists). Called after the plugin state was restored.
  std::function<void()> refresh_plugin_list;
  // Reload the Archives tab (BSA/BA2 list).
  std::function<void()> refresh_bsa_list;
  // Toggle archive invalidation (game INI bInvalidateOlderFiles) per the
  // new profile's AutomaticArchiveInvalidation setting (MO2's
  // activateInvalidation / deactivateInvalidation).
  std::function<void(bool active)> set_archive_invalidation;
};

// Persist the current profile's state (MO2's saveCurrentProfile):
//   1. modlist.txt flushed immediately (write_modlist_now — the in-memory
//      mod list is the source of truth; the delayed writer never holds a
//      switch hostage).
//   2. plugins.txt / loadorder.txt / lockedorder.txt via
//      PluginDatabase::save_profile when `plugin_db` is set (the live plugin
//      state is the source of truth; skipped when null).
//   3. archives.txt (always written, may be empty).
//   4. initweaks.ini when state.tweaked_ini is non-empty.
//   5. settings.ini.
// Returns false with *error set on the first failure.
bool save_current_profile(ProfileManager &profile, const ProfileSaveState &state,
                          engine::PluginDatabase *plugin_db = nullptr,
                          std::string *error = nullptr);

// Atomically write the profile's tweaked INI file (initweaks.ini). The
// content is the merged INI tweaks of the enabled mods + profile tweaks
// (MO2's createTweakedIniFile); the engine writes what the caller gathered.
bool write_tweaked_ini(const std::filesystem::path &profile_dir,
                       const std::string &content,
                       std::string *error = nullptr);

// Switch the active profile to `name` under profiles_dir (MO2's
// OrganizerCore::setCurrentProfile). Handles the full transition:
//
//   1. No-op when `name` is already the current profile's name.
//   2. Resolve the real profile directory (case-insensitive walk — the UI
//      combo box walks directories on its own and may pass a differently
//      cased name).
//   3. Save the current profile (when `current` is non-null) via
//      save_current_profile with `current_state`.
//   4. Construct the new Profile from its directory.
//   5. Restore mod enable/disable + priorities from the new profile's
//      modlist.txt (refresh_mod_status — the "set profile on ModList" step;
//      the UI rebuilds its ModList in refresh_directory_structure).
//   6. Restore plugin state (plugins.txt / loadorder.txt / lockedorder.txt)
//      via PluginDatabase::load_profile when `plugin_db` is set.
//   7. Activate/deactivate archive invalidation per the new profile's
//      AutomaticArchiveInvalidation setting.
//   8. Refresh directory structure, plugin list and BSA list via the
//      callbacks.
//   9. Emit the profile_changed event on the EventBus (P1.3, mirrors MO2's
//      onProfileChanged).
//
// On failure the current profile is left untouched (the save happens before
// the new Profile is constructed) and the result carries a human-readable
// error.
ProfileSwitchResult switch_profile(const std::filesystem::path &profiles_dir,
                                   const std::string &name, ProfileManager *current,
                                   const ProfileSaveState &current_state,
                                   engine::PluginDatabase *plugin_db,
                                   const ProfileSwitchCallbacks &callbacks);

} // namespace engine::profile