#include "engine/profile/profile_creation.h"

#include "engine/core/log/logger.h"
#include "engine/profile/profile.h"
#include "engine/profile/safe_write_file.h"

#include <algorithm>
#include <system_error>

namespace engine::profile {

namespace {

// Remove a partially-created profile directory, logging the failure. Never
// throws (error_code overloads).
void cleanup_failed(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    if (ec) {
        Logger::instance().error("failed to clean up profile directory " + dir.string() + ": " +
                                 ec.message());
    }
}

// True when `path` names a game INI inside a profile directory — any *.ini
// other than the profile's own settings.ini (MO2's findProfileSettings
// detects local settings by the presence of the game's INI files).
bool is_game_ini(const std::filesystem::path& path) {
    return path.extension() == ".ini" && path.filename() != "settings.ini";
}

// MO2's findProfileSettings, reduced to what the engine can observe without
// a game plugin: a saves/ subdir implies LocalSaves=true, a _saves subdir
// implies false; a game INI file in the profile implies LocalSettings=true.
// The caller writes the defaults first, then this overrides what the
// directory state proves (MO2 only detects when the setting is unset, which
// is the case for a fresh profile).
void detect_local_settings(Profile& profile) {
    const auto dir = profile.directory();

    if (std::filesystem::is_directory(dir / "saves")) {
        profile.set_local_saves(true);
    } else if (std::filesystem::is_directory(dir / "_saves")) {
        profile.set_local_saves(false);
    }

    bool has_game_ini = false;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (is_game_ini(it->path())) {
            has_game_ini = true;
            break;
        }
    }
    if (has_game_ini) {
        profile.set_local_settings(true);
    }
}

}  // namespace

bool is_valid_profile_name(const std::string& name, std::string* error) {
    if (name.empty()) {
        if (error) {
            *error = "profile name is empty";
        }
        return false;
    }
    if (name == "." || name == "..") {
        if (error) {
            *error = "profile name must not be \".\" or \"..\"";
        }
        return false;
    }
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
        if (error) {
            *error = "profile name must not contain path separators";
        }
        return false;
    }
    if (name.find('\0') != std::string::npos) {
        if (error) {
            *error = "profile name must not contain NUL";
        }
        return false;
    }
    return true;
}

ProfileCreationResult create_fresh_profile(const std::filesystem::path& profiles_dir, const std::string& name,
                                           ProfileGameInitFn game_init) {
    ProfileCreationResult result;

    std::string name_error;
    if (!is_valid_profile_name(name, &name_error)) {
        result.error = name_error;
        return result;
    }

    const auto target = profiles_dir / name;
    if (std::filesystem::exists(target)) {
        result.error = "profile \"" + name + "\" already exists";
        return result;
    }

    std::error_code ec;
    std::filesystem::create_directories(target, ec);
    if (ec) {
        result.error = "failed to create profile directory " + target.string() + ": " + ec.message();
        Logger::instance().error(result.error);
        return result;
    }

    // Create the base files. Empty modlist.txt / archives.txt first (MO2's
    // touchFile), then let the game plugin initialize game-specific files,
    // then detect LocalSaves/LocalSettings from the resulting directory
    // state, then write settings.ini with the detected values.
    if (!safe_write_file(target / "modlist.txt", "")) {
        result.error = "failed to create modlist.txt in " + target.string();
        Logger::instance().error(result.error);
        cleanup_failed(target);
        return result;
    }
    if (!safe_write_file(target / "archives.txt", "")) {
        result.error = "failed to create archives.txt in " + target.string();
        Logger::instance().error(result.error);
        cleanup_failed(target);
        return result;
    }

    if (game_init) {
        game_init(target);
    }

    Profile profile(target);
    // Defaults first (MO2's useDefaultSettings / PREFER_DEFAULTS), then
    // auto-detection overrides what the directory state proves.
    profile.set_local_saves(false);
    profile.set_local_settings(false);
    profile.set_automatic_archive_invalidation(false);
    detect_local_settings(profile);

    if (!profile.save_settings()) {
        result.error = "failed to write settings.ini in " + target.string();
        cleanup_failed(target);
        return result;
    }

    result.success = true;
    result.directory = target;
    return result;
}

ProfileCreationResult copy_profile(const std::filesystem::path& profiles_dir, const std::string& new_name,
                                   const std::filesystem::path& source_dir) {
    ProfileCreationResult result;

    std::string name_error;
    if (!is_valid_profile_name(new_name, &name_error)) {
        result.error = name_error;
        return result;
    }

    std::error_code ec;
    if (!std::filesystem::is_directory(source_dir, ec) || ec) {
        result.error = "source profile directory " + source_dir.string() + " does not exist";
        return result;
    }

    const auto target = profiles_dir / new_name;
    if (std::filesystem::exists(target)) {
        result.error = "profile \"" + new_name + "\" already exists";
        return result;
    }

    std::filesystem::create_directories(profiles_dir, ec);
    if (ec) {
        result.error = "failed to create profiles directory " + profiles_dir.string() + ": " + ec.message();
        Logger::instance().error(result.error);
        return result;
    }

    // Recursive copy preserving symlinks (a profile may symlink saves/ or
    // game INIs); the copy fails closed on any error.
    std::filesystem::copy(
        source_dir, target,
        std::filesystem::copy_options::recursive | std::filesystem::copy_options::copy_symlinks, ec);
    if (ec) {
        result.error = "failed to copy profile from " + source_dir.string() + " to " + target.string() +
                       ": " + ec.message();
        Logger::instance().error(result.error);
        cleanup_failed(target);
        return result;
    }

    // Record the new profile name in settings.ini (the directory name is the
    // profile identity; the key makes the copy self-describing). All other
    // settings — LocalSaves, LocalSettings, AutomaticArchiveInvalidation,
    // forced_libraries, unknown keys — are preserved verbatim by the copy.
    Profile profile(target);
    profile.set_root_setting("ProfileName", new_name);
    if (!profile.save_settings()) {
        result.error = "failed to update settings.ini in " + target.string();
        cleanup_failed(target);
        return result;
    }

    result.success = true;
    result.directory = target;
    return result;
}

bool rename_profile(const std::filesystem::path& profiles_dir, const std::string& old_name,
                    const std::string& new_name, std::string* error) {
    std::string name_error;
    if (!is_valid_profile_name(new_name, &name_error)) {
        if (error) {
            *error = name_error;
        }
        return false;
    }

    const auto source = profiles_dir / old_name;
    const auto target = profiles_dir / new_name;

    std::error_code ec;
    if (!std::filesystem::is_directory(source, ec) || ec) {
        const std::string msg = "source profile directory " + source.string() + " does not exist";
        if (error) {
            *error = msg;
        }
        return false;
    }
    if (std::filesystem::exists(target, ec)) {
        const std::string msg = "profile \"" + new_name + "\" already exists";
        if (error) {
            *error = msg;
        }
        return false;
    }

    std::filesystem::rename(source, target, ec);
    if (ec) {
        const std::string msg =
            "failed to rename profile to " + target.string() + ": " + ec.message();
        Logger::instance().error(msg);
        if (error) {
            *error = msg;
        }
        return false;
    }

    // Record the new name in settings.ini (the directory name is the profile
    // identity; the key makes the rename self-describing).
    Profile profile(target);
    profile.set_root_setting("ProfileName", new_name);
    if (!profile.save_settings()) {
        const std::string msg = "failed to update settings.ini in " + target.string();
        Logger::instance().error(msg);
        if (error) {
            *error = msg;
        }
        return false;
    }
    return true;
}

std::vector<std::string> list_profiles(const std::filesystem::path& profiles_dir) {
    std::vector<std::string> names;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(profiles_dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_directory(ec) && !ec) {
            names.push_back(it->path().filename().string());
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

}  // namespace engine::profile