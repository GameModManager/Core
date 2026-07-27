#include "runtime/runtime.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace engine {

// --- NativeRuntime ---

bool NativeRuntime::launch(const std::filesystem::path& executable,
                           const std::filesystem::path& /*game_dir*/,
                           uint32_t /*steam_appid*/) {
    if (!std::filesystem::exists(executable)) return false;

    std::string cmd = "\"" + executable.string() + "\" &";
    return std::system(cmd.c_str()) == 0;
}

bool NativeRuntime::is_available() const {
    return true;
}

// --- ProtonRuntime ---

// Find Steam root by checking common paths
static std::filesystem::path find_steam_root() {
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

// Parse a VDF value for a given key from a line
static std::string vdf_value_for_key(const std::string& line, const std::string& key) {
    auto pos = line.find("\"" + key + "\"");
    if (pos == std::string::npos) return {};

    // Skip past the key
    pos += key.size() + 2; // +2 for surrounding quotes

    // Find the value (between next pair of quotes)
    auto q1 = line.find('"', pos);
    if (q1 == std::string::npos) return {};
    auto q2 = line.find('"', q1 + 1);
    if (q2 == std::string::npos) return {};

    return line.substr(q1 + 1, q2 - q1 - 1);
}

// Read the per-game Proton tool override from Steam's config.vdf
// Returns the tool name (e.g. "proton_experimental") or empty
static std::string read_steam_compat_tool(uint32_t appid) {
    auto steam_root = find_steam_root();
    if (steam_root.empty()) return {};

    // Try config/config.vdf first (newer Steam layouts)
    auto config_path = steam_root / "config" / "config.vdf";
    if (!std::filesystem::exists(config_path)) {
        // Fallback: some distros put config.vdf at the root
        config_path = steam_root / "config.vdf";
    }
    if (!std::filesystem::exists(config_path)) return {};

    std::ifstream f(config_path);
    if (!f) return {};

    std::string appid_str = std::to_string(appid);
    std::string line;
    bool in_compat_overrides = false;
    bool in_app_section = false;

    while (std::getline(f, line)) {
        // Trim leading whitespace for indent tracking
        auto trimmed = line;
        auto start = trimmed.find_first_not_of(" \t");
        if (start != std::string::npos) trimmed = trimmed.substr(start);

        // Look for "CompatToolOverrides"
        if (trimmed.find("CompatToolOverrides") != std::string::npos) {
            in_compat_overrides = true;
            continue;
        }

        if (in_compat_overrides) {
            // Look for the app ID section
            if (trimmed.find('"' + appid_str + '"') != std::string::npos) {
                in_app_section = true;
                continue;
            }

            if (in_app_section) {
                // Look for "name" key — this is the tool name
                auto tool_name = vdf_value_for_key(trimmed, "name");
                if (!tool_name.empty()) {
                    return tool_name;
                }
                // If we hit a closing brace at the same indent level, we've left the section
                if (trimmed == "}") {
                    break;
                }
            }
        }
    }

    return {};
}

// Map a tool name (e.g. "proton_experimental") to its install directory
// by reading compatibilitytool.vdf
static std::filesystem::path resolve_tool_dir(const std::string& tool_name) {
    auto steam_root = find_steam_root();
    if (steam_root.empty()) return {};

    auto vdf_path = steam_root / "steamapps" / "compatibilitytool.vdf";
    if (!std::filesystem::exists(vdf_path)) return {};

    std::ifstream f(vdf_path);
    if (!f) return {};

    std::string line;
    bool in_tool_section = false;

    while (std::getline(f, line)) {
        auto trimmed = line;
        auto start = trimmed.find_first_not_of(" \t");
        if (start != std::string::npos) trimmed = trimmed.substr(start);

        // Look for a section matching the tool name
        if (trimmed.find('"' + tool_name + '"') != std::string::npos) {
            in_tool_section = true;
            continue;
        }

        if (in_tool_section) {
            auto install_path = vdf_value_for_key(trimmed, "install_path");
            if (!install_path.empty()) {
                return std::filesystem::path(install_path);
            }
            if (trimmed == "}") {
                break;
            }
        }
    }

    return {};
}

// Find the latest Proton installation alphabetically (fallback)
static std::filesystem::path find_latest_proton() {
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

// Find the Proton binary for a specific game, respecting Steam's per-game override
static std::filesystem::path find_proton_for_game(uint32_t appid) {
    // 1. Check Steam's per-game compat tool override
    auto tool_name = read_steam_compat_tool(appid);
    if (!tool_name.empty()) {
        auto tool_dir = resolve_tool_dir(tool_name);
        if (!tool_dir.empty()) {
            // The proton binary can be directly in the tool dir or in a subdirectory
            auto proton_bin = tool_dir / "proton";
            if (std::filesystem::exists(proton_bin)) {
                return proton_bin;
            }
            // Some tools have the proton script in a versioned subdirectory
            if (std::filesystem::exists(tool_dir)) {
                for (const auto& entry : std::filesystem::directory_iterator(tool_dir)) {
                    if (entry.is_directory()) {
                        auto sub_proton = entry.path() / "proton";
                        if (std::filesystem::exists(sub_proton)) {
                            return sub_proton;
                        }
                    }
                }
            }
        }
    }

    // 2. Fallback: find the latest Proton installation
    return find_latest_proton();
}

bool ProtonRuntime::launch(const std::filesystem::path& executable,
                           const std::filesystem::path& game_dir,
                           uint32_t steam_appid) {
    if (!std::filesystem::exists(executable)) return false;

    auto proton = find_proton_for_game(steam_appid);
    if (proton.empty()) {
        proton = find_proton();
        if (proton.empty()) return false;
    }

    auto steam_root = find_steam_root();
    if (steam_root.empty()) return false;

    // Set required Steam environment variables for the proton script
    auto compat_data = steam_root / "steamapps" / "compatdata" / std::to_string(steam_appid);
    setenv("STEAM_COMPAT_DATA_PATH", compat_data.string().c_str(), 1);
    setenv("STEAM_COMPAT_CLIENT_INSTALL_PATH", steam_root.string().c_str(), 1);
    setenv("STEAM_COMPAT_INSTALL_PATH", game_dir.string().c_str(), 1);
    setenv("STEAM_COMPAT_APP_ID", std::to_string(steam_appid).c_str(), 1);

    // Build library paths — all Steam library folders
    std::string library_paths;
    auto lib_folders = steam_root / "steamapps" / "libraryfolders.vdf";
    if (std::filesystem::exists(lib_folders)) {
        std::ifstream lf(lib_folders);
        std::string line;
        while (std::getline(lf, line)) {
            auto val = vdf_value_for_key(line, "path");
            if (!val.empty()) {
                if (!library_paths.empty()) library_paths += ":";
                library_paths += val;
            }
        }
    }
    if (library_paths.empty()) {
        library_paths = steam_root.string();
    }
    setenv("STEAM_COMPAT_LIBRARY_PATHS", library_paths.c_str(), 1);

    std::string cmd = "\"" + proton.string() + "\" waitforexitandrun \"" + executable.string() + "\" &";
    return std::system(cmd.c_str()) == 0;
}

bool ProtonRuntime::is_available() const {
    return !find_proton().empty();
}

std::filesystem::path ProtonRuntime::find_proton() const {
    return find_latest_proton();
}

}  // namespace engine
