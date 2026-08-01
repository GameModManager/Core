#pragma once

#include <filesystem>
#include <string>
#include <vector>

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

    // Proton discovery - returns path to proton script/binary, empty if unavailable.
    // On Windows this always returns empty (no Proton on Windows).
    // On Linux this searches Steam tools for Proton installations.
    [[nodiscard]] virtual std::filesystem::path find_proton() const { return {}; }

    // Per-game Proton runner selection, respecting Steam's per-game compat
    // tool override. Falls back to the latest Proton when no override exists.
    [[nodiscard]] virtual std::filesystem::path find_proton_for_game(
        uint32_t steam_appid) const { return find_proton(); }

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
};

}  // namespace engine
