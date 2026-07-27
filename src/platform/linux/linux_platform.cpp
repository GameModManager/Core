#include "platform/linux/linux_platform.h"

#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

namespace engine {

// --- XDG Base Directory resolution ---

std::filesystem::path LinuxPlatform::resolve_env_dir(
    const char* env_var, const std::filesystem::path& fallback) {
    auto val = std::getenv(env_var);
    if (val && val[0] != '\0') {
        return std::filesystem::path(val);
    }
    return fallback;
}

std::filesystem::path LinuxPlatform::data_dir() const {
    auto home = std::getenv("HOME");
    if (!home) return "/tmp/gamemodmanager";
    return resolve_env_dir("XDG_DATA_HOME",
                           std::filesystem::path(home) / ".local" / "share") /
           "gamemodmanager";
}

std::filesystem::path LinuxPlatform::config_dir() const {
    auto home = std::getenv("HOME");
    if (!home) return "/tmp/gamemodmanager";
    return resolve_env_dir("XDG_CONFIG_HOME",
                           std::filesystem::path(home) / ".config") /
           "gamemodmanager";
}

std::filesystem::path LinuxPlatform::cache_dir() const {
    auto home = std::getenv("HOME");
    if (!home) return "/tmp/gamemodmanager";
    return resolve_env_dir("XDG_CACHE_HOME",
                           std::filesystem::path(home) / ".cache") /
           "gamemodmanager";
}

// --- Steam discovery ---

std::filesystem::path LinuxPlatform::find_steam_root() const {
    auto home = std::getenv("HOME");
    if (!home) return {};

    std::filesystem::path home_path(home);
    std::vector<std::filesystem::path> candidates = {
        home_path / ".local" / "share" / "Steam",
        home_path / ".steam" / "steam",
        home_path / ".steam" / "debian-installation",
    };

    for (const auto& root : candidates) {
        auto vdf = root / "steamapps" / "libraryfolders.vdf";
        if (std::filesystem::exists(vdf)) {
            return root;
        }
    }
    return {};
}

// --- Proton discovery ---

std::filesystem::path LinuxPlatform::find_proton() const {
    auto steam_root = find_steam_root();
    if (steam_root.empty()) return {};

    auto tools = steam_root / "steamapps" / "common";
    if (!std::filesystem::exists(tools)) return {};

    std::filesystem::path best;
    for (const auto& entry : std::filesystem::directory_iterator(tools)) {
        if (!entry.is_directory()) continue;
        auto name = entry.path().filename().string();
        if (name.find("Proton") != std::string::npos) {
            auto proton_bin = entry.path() / "proton";
            if (std::filesystem::exists(proton_bin)) {
                if (best.empty() || name > best.filename().string()) {
                    best = proton_bin;
                }
            }
        }
    }
    return best;
}

// --- Wine discovery ---

std::filesystem::path LinuxPlatform::find_wine() const {
    auto* path = std::getenv("PATH");
    if (path) {
        std::istringstream ss(path);
        std::string token;
        while (std::getline(ss, token, ':')) {
            auto wine_path = std::filesystem::path(token) / "wine";
            if (std::filesystem::exists(wine_path)) {
                return wine_path;
            }
        }
    }

    std::vector<std::filesystem::path> candidates = {
        "/usr/bin/wine",
        "/usr/local/bin/wine",
        "/opt/wine/bin/wine",
    };

    for (const auto& c : candidates) {
        if (std::filesystem::exists(c)) return c;
    }
    return {};
}

// --- Process launch ---

bool LinuxPlatform::launch_executable(
    const std::filesystem::path& executable,
    const std::vector<std::string>& args) const {
    if (!std::filesystem::exists(executable)) return false;

    std::string cmd = "\"" + executable.string() + "\"";
    for (const auto& arg : args) {
        cmd += " \"" + arg + "\"";
    }
    cmd += " &";

    return std::system(cmd.c_str()) == 0;
}

// --- Privilege check ---

bool LinuxPlatform::is_elevated() const {
    return geteuid() == 0;
}

}  // namespace engine
