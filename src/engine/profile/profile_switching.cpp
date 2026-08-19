#include "engine/profile/profile_switching.h"

#include "engine/core/events/event_bus.h"
#include "engine/core/log/logger.h"
#include "engine/game/plugins/plugin_database.h"
#include "engine/profile/safe_write_file.h"

#include <algorithm>
#include <cctype>
#include <system_error>

namespace engine::profile {

namespace {

// Case-insensitive string equality (profile names are compared
// case-insensitively — MO2 walks directories and matches with
// Qt::CaseInsensitive).
bool iequals(const std::string &a, const std::string &b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

} // namespace

bool write_tweaked_ini(const std::filesystem::path &profile_dir,
                       const std::string &content, std::string *error) {
  if (!safe_write_file(profile_dir / "initweaks.ini", content)) {
    const std::string message =
        "failed to write initweaks.ini in " + profile_dir.string();
    if (error) {
      *error = message;
    }
    Logger::instance().error(message);
    return false;
  }
  return true;
}

bool save_current_profile(Profile &profile, const ProfileSaveState &state,
                          engine::PluginDatabase *plugin_db,
                          std::string *error) {
  // 1. Flush modlist.txt immediately. The in-memory mod list is the source
  //    of truth (the UI converges it with the mods dir via
  //    refresh_mod_status at scan time); the delayed writer must never hold
  //    a profile switch hostage (MO2's writeModlistNow(true) in
  //    refreshDirectoryStructure).
  profile.write_modlist_now();

  // 2. Plugin state (plugins.txt / loadorder.txt / lockedorder.txt) from
  //    the live plugin database — the in-memory plugin list is the source
  //    of truth, not per-toggle disk reads.
  if (plugin_db != nullptr) {
    const auto profiles_dir = profile.directory().parent_path();
    plugin_db->save_profile(profiles_dir, profile.name());
  }

  // 3. archives.txt (always written; an empty list is a valid state).
  if (!profile.write_archives(state.archives)) {
    const std::string message =
        "failed to write archives.txt in " + profile.directory().string();
    if (error) {
      *error = message;
    }
    Logger::instance().error(message);
    return false;
  }

  // 4. Tweaked INI file (initweaks.ini) — only when the caller gathered
  //    tweaks (MO2's createTweakedIniFile "if needed").
  if (!state.tweaked_ini.empty() &&
      !write_tweaked_ini(profile.directory(), state.tweaked_ini, error)) {
    return false;
  }

  // 5. settings.ini (LocalSaves / LocalSettings /
  //    AutomaticArchiveInvalidation and any unknown preserved keys).
  if (!profile.save_settings()) {
    const std::string message =
        "failed to write settings.ini in " + profile.directory().string();
    if (error) {
      *error = message;
    }
    Logger::instance().error(message);
    return false;
  }

  return true;
}

ProfileSwitchResult switch_profile(const std::filesystem::path &profiles_dir,
                                   const std::string &name, Profile *current,
                                   const ProfileSaveState &current_state,
                                   engine::PluginDatabase *plugin_db,
                                   const ProfileSwitchCallbacks &callbacks) {
  ProfileSwitchResult result;

  // No-op when the requested profile is already active (MO2's early return
  // in setCurrentProfile).
  if (current != nullptr && current->name() == name) {
    result.success = true;
    result.changed = false;
    return result;
  }

  // Resolve the real profile directory. The profile name may not have the
  // correct case (the UI combo box walks directories on its own), so walk
  // the profiles dir and match case-insensitively (MO2 parity).
  std::filesystem::path profile_dir;
  std::error_code ec;
  for (std::filesystem::directory_iterator it(profiles_dir, ec), end;
       !ec && it != end; it.increment(ec)) {
    if (!it->is_directory(ec) || ec) {
      continue;
    }
    if (iequals(it->path().filename().string(), name)) {
      profile_dir = it->path();
      break;
    }
  }
  if (profile_dir.empty()) {
    result.error = "profile \"" + name + "\" does not exist";
    Logger::instance().error("profile switch: " + result.error);
    return result;
  }

  // Save the current profile's state before switching away. On failure the
  // current profile is left untouched — the new Profile is only constructed
  // after the save succeeded.
  if (current != nullptr) {
    std::string save_error;
    if (!save_current_profile(*current, current_state, plugin_db,
                              &save_error)) {
      result.error = "failed to save current profile: " + save_error;
      Logger::instance().error("profile switch: " + result.error);
      return result;
    }
  }

  // Construct the new Profile from its directory (MO2's
  // make_unique<Profile>(profileDir, ...)).
  auto new_profile = std::make_unique<Profile>(profile_dir);

  // Restore mod enable/disable + priorities from the new profile's
  // modlist.txt (MO2's refreshModStatus on profile load). This is the
  // "set profile on ModList" step: the Profile's in-memory mod list now
  // mirrors the new profile, and the UI rebuilds its ModListModel in the
  // refresh_directory_structure callback.
  new_profile->refresh_mod_status(current_state.known_mods,
                                  current_state.foreign_mods);

  // Restore plugin state (plugins.txt / loadorder.txt / lockedorder.txt)
  // into the live plugin database.
  if (plugin_db != nullptr) {
    bool repaired = false;
    plugin_db->load_profile(profiles_dir, new_profile->name(), &repaired);
    if (repaired) {
      Logger::instance().debug(
          "profile switch: load order repaired for profile '" +
          new_profile->name() + "'");
    }
  }

  // Activate/deactivate archive invalidation per the new profile's
  // AutomaticArchiveInvalidation setting (MO2's activateInvalidation /
  // deactivateInvalidation).
  if (callbacks.set_archive_invalidation) {
    callbacks.set_archive_invalidation(
        new_profile->automatic_archive_invalidation());
  }

  // Refresh the UI-owned views (MO2's refreshDirectoryStructure +
  // refreshLists).
  if (callbacks.refresh_directory_structure) {
    callbacks.refresh_directory_structure();
  }
  if (callbacks.refresh_plugin_list) {
    callbacks.refresh_plugin_list();
  }
  if (callbacks.refresh_bsa_list) {
    callbacks.refresh_bsa_list();
  }

  // P1.3 event bus: mirror MO2's onProfileChanged.
  EventBus::instance().dispatch(
      events::kProfileChanged,
      json_obj({{"profile", new_profile->name()},
                {"old_profile", current != nullptr ? current->name() : ""}}));

  result.success = true;
  result.changed = true;
  result.profile = std::move(new_profile);
  return result;
}

} // namespace engine::profile