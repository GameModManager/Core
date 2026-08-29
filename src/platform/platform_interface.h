#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace engine {

// Platform abstraction layer. Each OS provides its own implementation.
// The engine never calls OS-specific APIs directly - always through this interface.
//
// Owned by main.cpp, passed to anything that needs platform services.
// Qt-free: lives in engine/, no Qt headers.
class PlatformInterface {
public:
    virtual ~PlatformInterface() = default;

    // Identity
    [[nodiscard]] virtual std::string platform_name() const = 0;

    // Directory resolution per XDG / Windows conventions
    [[nodiscard]] virtual std::filesystem::path data_dir() const = 0;
    [[nodiscard]] virtual std::filesystem::path config_dir() const = 0;
    [[nodiscard]] virtual std::filesystem::path cache_dir() const = 0;

    // Instances directory (under data_dir by default)
    [[nodiscard]] virtual std::filesystem::path instances_dir() const {
        return data_dir() / "instances";
    }

    // Steam discovery - platform-specific paths
    [[nodiscard]] virtual std::filesystem::path find_steam_root() const = 0;

    // A discovered Proton runner: user-facing name + path to the `proton`
    // script/binary. The name is what Steam/the user knows it as and what is
    // persisted as a per-instance runner override.
    struct ProtonVersionInfo {
        std::string name;
        std::filesystem::path binary;
    };

    // Proton discovery - returns path to proton script/binary, empty if unavailable.
    // On Windows this always returns empty (no Proton on Windows).
    // On Linux this searches Steam tools for Proton installations.
    [[nodiscard]] virtual std::filesystem::path find_proton() const { return {}; }

    // Per-game Proton runner selection, respecting Steam's per-game compat
    // tool override. Falls back to the latest Proton when no override exists.
    [[nodiscard]] virtual std::filesystem::path find_proton_for_game(
        uint32_t steam_appid) const { return find_proton(); }

    // Every installed Proton runner (name -> proton binary), for the UI's
    // runner selector. Empty on platforms without Proton.
    [[nodiscard]] virtual std::vector<ProtonVersionInfo>
    enumerate_proton_versions() const { return {}; }

    // Resolve a named Proton runner (as persisted in instance.toml) to its
    // proton binary. `name` may be an absolute path (used directly) or the
    // runner's display name (searched among installed runners). Empty when
    // the runner cannot be found.
    [[nodiscard]] virtual std::filesystem::path find_proton_named(
        const std::string& name) const { return {}; }

    // Proton prefix (compatdata) directory for a game. Empty when not
    // applicable (no Steam, or the platform has no Proton).
    [[nodiscard]] virtual std::filesystem::path resolve_proton_prefix(
        uint32_t steam_appid) const { return {}; }

    // All Steam library folders (paths from libraryfolders.vdf, in priority
    // order). Empty when not applicable. Used to build STEAM_COMPAT_LIBRARY_PATHS.
    [[nodiscard]] virtual std::vector<std::filesystem::path>
    steam_library_paths() const { return {}; }

    // Windows user "Documents" directory for a game running under this
    // platform's prefix. On Linux this is inside the Proton prefix
    // (drive_c/users/<user>/Documents); on Windows the native
    // %USERPROFILE%\Documents. Empty when not applicable.
    [[nodiscard]] virtual std::filesystem::path game_documents_dir(
        uint32_t steam_appid) const { return {}; }

    // Windows user "Local AppData" directory for a game running under this
    // platform's prefix. On Linux this is inside the Proton prefix
    // (drive_c/users/<user>/AppData/Local); on Windows the native
    // %LOCALAPPDATA%. Empty when not applicable.
    [[nodiscard]] virtual std::filesystem::path game_local_appdata_dir(
        uint32_t steam_appid) const { return {}; }

    // Wine discovery - for launching Windows games on Linux without Proton.
    // Returns path to wine binary, empty if unavailable.
    [[nodiscard]] virtual std::filesystem::path find_wine() const { return {}; }

    // Launch a game executable. Platform handles the actual process creation.
    [[nodiscard]] virtual bool launch_executable(
        const std::filesystem::path& executable,
        const std::vector<std::string>& args = {}) const = 0;

    // Check if the current user has elevated/admin privileges.
    [[nodiscard]] virtual bool is_elevated() const { return false; }

    // Check if symlinks are available (requires privileges on Windows).
    [[nodiscard]] virtual bool symlinks_available() const { return true; }

    // Check if NTFS junctions are available (Windows only, always true there).
    [[nodiscard]] virtual bool junctions_available() const { return false; }

    // Home directory — Linux/macOS: $HOME, Windows: %USERPROFILE%
    [[nodiscard]] virtual std::filesystem::path home_dir() const = 0;

    // Temporary directory
    [[nodiscard]] virtual std::filesystem::path temp_dir() const = 0;

    // Lower the current thread's CPU priority
    virtual void set_thread_low_priority() const {}
};

// Centralized home dir lookup for code paths without a PlatformInterface pointer.
inline std::filesystem::path safe_home_dir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH + 1];
    if (GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH))
        return std::filesystem::path(buf);
#else
    if (const char* home = std::getenv("HOME"))
        return std::filesystem::path(home);
#endif
    return std::filesystem::temp_directory_path();
}

}  // namespace engine
