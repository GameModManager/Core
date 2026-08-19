#pragma once

#include "engine/profile/delayed_file_writer.h"

#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace engine::profile {

// One entry of the profile's mod list (modlist.txt).
//
// mod_id is the mod's on-disk folder name (mod identity — never the display
// name). priority is 0-based; higher priority wins conflicts and is written
// FIRST in modlist.txt (MO2 convention: the file's first line is the highest
// priority mod).
struct ModListEntry {
    std::string mod_id;
    bool enabled = true;   // '+' enabled / '-' disabled
    bool foreign = false;  // '*' unmanaged (DLC etc.), never written as +/- toggle
    int priority = 0;
};

// One locked plugin priority entry (lockedorder.txt, "name|priority").
struct LockedPlugin {
    std::string name;
    int priority = 0;
};

// Outcome of Profile::remove(). The engine is Qt-free and never shows
// dialogs: the caller (UI) is expected to confirm with the user BEFORE
// calling remove() and to react to the result afterwards (MO2 shows a
// QMessageBox before deleting; the result enum replaces the dialog's
// success/failure handling).
enum class ProfileRemoveResult {
    Removed,          // directory and all contents deleted
    NotFound,         // directory did not exist (nothing to delete)
    ActiveProfile,    // refused: the profile is the active one
    PermissionDenied, // could not delete (permissions, locked files, I/O error)
    PartialFailure,   // some contents deleted, the directory still exists
};

// Ordered INI model of settings.ini (root section first, keys in file order).
// Unknown keys/sections are preserved on save (read-before-write — never
// overwrite a shared config with a partial view).
struct IniSection {
    std::string name;  // empty = root section (before any [header])
    std::vector<std::pair<std::string, std::string>> entries;
};

// Profile directory manager — the core data model of the profile system.
//
// A profile is a directory holding:
//   settings.ini    QSettings INI format (LocalSaves, LocalSettings,
//                   AutomaticArchiveInvalidation)
//   modlist.txt     "+Name" enabled / "-Name" disabled / "*Name" foreign,
//                   first line = highest priority
//   plugins.txt     plugin enable/disable (game-specific)
//   loadorder.txt   plugin load order (game-specific)
//   lockedorder.txt locked plugin priorities ("name|priority")
//   archives.txt    BSA/BA2 enable/disable (game-specific)
//
// The engine is Qt-free, so settings.ini is handled by a small INI
// reader/writer instead of QSettings, and the modlist debounce (~5s) is
// implemented by DelayedFileWriter's background thread instead of a QTimer.
// All file writes go through safe_write_file (temp file + atomic rename).
//
// The mod list is mutated from the UI thread (set_mod_enabled,
// set_mod_priority) and read by the DelayedFileWriter thread
// (do_write_modlist); mods_ is protected by mods_mutex_.
//
// NOTE: this class is distinct from engine::Profile (mod/model/profile.h),
// which is the in-memory mod-list model exposed to plugins via the ABI
// bridge. This class owns the profile DIRECTORY and its files.
class Profile {
public:
    // Default debounce delay for modlist.txt writes (MO2 parity).
    static constexpr std::chrono::milliseconds kModlistWriteDelay{5000};

    // Construct from an existing profile directory. The directory is NOT
    // created here (that is profile creation, a separate concern); read
    // methods return empty state and write methods create the directory on
    // demand. `modlist_delay` is the DelayedFileWriter debounce (tests pass a
    // short delay).
    explicit Profile(std::filesystem::path directory,
                     std::chrono::milliseconds modlist_delay = kModlistWriteDelay);
    ~Profile();

    Profile(const Profile&) = delete;
    Profile& operator=(const Profile&) = delete;
    Profile(Profile&&) = delete;
    Profile& operator=(Profile&&) = delete;

    // --- directory ---------------------------------------------------------

    [[nodiscard]] const std::filesystem::path& directory() const { return directory_; }
    [[nodiscard]] std::string name() const { return directory_.filename().string(); }
    [[nodiscard]] bool exists() const { return std::filesystem::exists(directory_); }

    // Permanently delete the profile directory and all its contents
    // (settings.ini, modlist.txt, plugins.txt, profile-specific save games,
    // ...) — MO2's "remove profile" behavior.
    //
    // is_active must be true when this profile is the currently active one:
    // deletion is then refused with ActiveProfile. The caller is expected to
    // check first (defense in depth — MO2 refuses in the dialog too).
    //
    // Any pending delayed modlist write is cancelled first so the
    // DelayedFileWriter thread never recreates files inside a directory that
    // is being deleted. After a successful removal the Profile object is a
    // shell: exists() returns false and the caller should drop it.
    //
    // The caller is responsible for removing the profile from any in-memory
    // list and cleaning up references (UI list, active-profile state).
    ProfileRemoveResult remove(bool is_active = false);

    // --- settings.ini ------------------------------------------------------
    // Values are cached in memory (loaded at construction) and persisted by
    // save_settings(). Unknown keys/sections are preserved on write
    // (read-before-write — never overwrite a shared config with a partial
    // view).

    [[nodiscard]] bool local_saves() const;
    void set_local_saves(bool value);

    [[nodiscard]] bool local_settings() const;
    void set_local_settings(bool value);

    [[nodiscard]] bool automatic_archive_invalidation() const;
    void set_automatic_archive_invalidation(bool value);

    // Atomically write settings.ini. Returns true on success.
    bool save_settings();

    // --- modlist.txt -------------------------------------------------------

    // Re-read modlist.txt and rebuild the mod list + priority map.
    //
    // known_mods is the full set of mod ids present in the instance's mods
    // directory; foreign_mods is the subset that is unmanaged (DLC etc.).
    // Mods in the file keep their file order (first line = highest priority).
    // Mods NOT in the file are appended: foreign mods get the lowest
    // priorities, managed (new) mods the highest, both enabled by default.
    // When new mods are added the file is rewritten (delayed, batched) so the
    // on-disk state converges — MO2's refreshModStatus behavior.
    void refresh_mod_status(const std::vector<std::string>& known_mods,
                            const std::vector<std::string>& foreign_mods = {});

    // Mods sorted by priority ascending (index 0 = lowest priority). Returns
    // a copy: the live collection is protected by mods_mutex_ and must never
    // be handed out by reference.
    [[nodiscard]] std::vector<ModListEntry> mods() const;

    // Priority of a mod, or -1 when the mod is not in the profile.
    [[nodiscard]] int priority_of(const std::string& mod_id) const;

    // Enable/disable a mod. Schedules a delayed modlist.txt write.
    void set_mod_enabled(const std::string& mod_id, bool enabled);

    // Move a mod to a new priority (0 = lowest). Clamped to the valid range;
    // other mods shift to keep priorities contiguous. Schedules a delayed
    // modlist.txt write. Returns true when the priority changed.
    bool set_mod_priority(const std::string& mod_id, int new_priority);

    // Schedule a delayed modlist.txt write (~5s, debounced).
    void write_modlist();
    // Flush a pending modlist.txt write immediately.
    void write_modlist_now();
    // Discard a pending modlist.txt write.
    void cancel_modlist_write();

    // --- plugins.txt / loadorder.txt / lockedorder.txt / archives.txt ------
    // Game-specific files; the game plugin interprets them. Generic
    // line-based I/O here, atomic writes.

    [[nodiscard]] std::vector<std::string> read_plugins() const;
    bool write_plugins(const std::vector<std::string>& plugins);

    [[nodiscard]] std::vector<std::string> read_load_order() const;
    bool write_load_order(const std::vector<std::string>& order);

    [[nodiscard]] std::vector<LockedPlugin> read_locked_order() const;
    bool write_locked_order(const std::vector<LockedPlugin>& locked);

    [[nodiscard]] std::vector<std::string> read_archives() const;
    bool write_archives(const std::vector<std::string>& archives);

    // --- file paths --------------------------------------------------------

    [[nodiscard]] std::filesystem::path settings_path() const { return directory_ / "settings.ini"; }
    [[nodiscard]] std::filesystem::path modlist_path() const { return directory_ / "modlist.txt"; }
    [[nodiscard]] std::filesystem::path plugins_path() const { return directory_ / "plugins.txt"; }
    [[nodiscard]] std::filesystem::path loadorder_path() const { return directory_ / "loadorder.txt"; }
    [[nodiscard]] std::filesystem::path lockedorder_path() const { return directory_ / "lockedorder.txt"; }
    [[nodiscard]] std::filesystem::path archives_path() const { return directory_ / "archives.txt"; }

private:
    // Runs on the DelayedFileWriter thread; serialized with the UI thread via
    // mods_mutex_.
    void do_write_modlist();

    // --- settings.ini helpers (INI model) ---
    [[nodiscard]] std::string get_setting(const std::string& key) const;
    void set_setting(const std::string& key, const std::string& value);
    [[nodiscard]] bool get_setting_bool(const std::string& key) const;
    void set_setting_bool(const std::string& key, bool value);

    std::filesystem::path directory_;

    // Ordered INI model of settings.ini (see IniSection above).
    std::vector<IniSection> ini_;

    // Mod list state — protected by mods_mutex_ (UI thread mutates,
    // DelayedFileWriter thread reads).
    mutable std::mutex mods_mutex_;
    std::vector<ModListEntry> mods_;

    DelayedFileWriter modlist_writer_;
};

}  // namespace engine::profile