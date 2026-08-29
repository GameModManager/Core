#ifndef __linux__
#error "This file should only be compiled on the correct platform"
#endif

#include "platform/linux/linux_platform.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <sys/resource.h>
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
    if (!home) return "/tmp/GameModManager";
    return resolve_env_dir("XDG_DATA_HOME",
                           std::filesystem::path(home) / ".local" / "share") /
           "GameModManager";
}

std::filesystem::path LinuxPlatform::config_dir() const {
    auto home = std::getenv("HOME");
    if (!home) return "/tmp/GameModManager";
    return resolve_env_dir("XDG_CONFIG_HOME",
                           std::filesystem::path(home) / ".config") /
           "GameModManager";
}

std::filesystem::path LinuxPlatform::cache_dir() const {
    auto home = std::getenv("HOME");
    if (!home) return "/tmp/GameModManager";
    return resolve_env_dir("XDG_CACHE_HOME",
                           std::filesystem::path(home) / ".cache") /
           "GameModManager";
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

std::vector<PlatformInterface::ProtonVersionInfo>
LinuxPlatform::scan_proton_runners() const {
    std::vector<ProtonVersionInfo> result;
    auto steam_root = find_steam_root();
    if (steam_root.empty()) return result;

    // 1. Dirs under steamapps/common with a `proton` script (official Proton
    //    releases, Experimental, Hotfix, and GE-Proton when Steam-managed).
    auto tools = steam_root / "steamapps" / "common";
    std::error_code ec;
    if (std::filesystem::exists(tools)) {
        for (const auto& entry : std::filesystem::directory_iterator(tools, ec)) {
            if (!entry.is_directory()) continue;
            auto name = entry.path().filename().string();
            if (name.find("Proton") == std::string::npos) continue;
            auto proton_bin = entry.path() / "proton";
            if (std::filesystem::exists(proton_bin)) {
                result.push_back({name, proton_bin});
            }
        }
        ec.clear();
    }

    // 2. Compatibility tools registered in compatibilitytool.vdf. These live
    //    outside steamapps/common (e.g. ~/Games/proton-ge-custom).
    auto vdf_path = steam_root / "steamapps" / "compatibilitytool.vdf";
    if (std::filesystem::exists(vdf_path)) {
        std::ifstream f(vdf_path);
        std::string line;
        std::string current_tool;
        while (std::getline(f, line)) {
            auto trimmed = line;
            auto start = trimmed.find_first_not_of(" \t");
            if (start != std::string::npos) trimmed = trimmed.substr(start);

            // A quoted token at the start of a line is a section header
            // (the tool name). Remember it so a following install_path
            // associates with the right tool.
            if (!trimmed.empty() && trimmed[0] == '"' && trimmed.back() == '"' &&
                trimmed.find('=') == std::string::npos &&
                trimmed.find("install_path") == std::string::npos) {
                auto close = trimmed.find('"', 1);
                if (close != std::string::npos) {
                    current_tool = trimmed.substr(1, close - 1);
                }
                continue;
            }

            auto install_path = vdf_value_for_key(trimmed, "install_path");
            if (!install_path.empty() && !current_tool.empty()) {
                auto proton_bin = std::filesystem::path(install_path) / "proton";
                if (std::filesystem::exists(proton_bin)) {
                    result.push_back({current_tool, proton_bin});
                }
                current_tool.clear();
            }
        }
    }

    return result;
}

std::filesystem::path LinuxPlatform::find_proton() const {
    auto runners = scan_proton_runners();
    if (runners.empty()) return {};

    std::string best_name;
    std::filesystem::path best;
    for (const auto& r : runners) {
        if (best.empty() || r.name > best_name) {
            best_name = r.name;
            best = r.binary;
        }
    }
    return best;
}

std::vector<PlatformInterface::ProtonVersionInfo>
LinuxPlatform::enumerate_proton_versions() const {
    return scan_proton_runners();
}

std::filesystem::path LinuxPlatform::find_proton_named(const std::string& name) const {
    if (name.empty()) return {};

    // Absolute (or relative-with-slash) path: use it directly.
    auto p = std::filesystem::path(name);
    if (name.find('/') != std::string::npos) {
        if (std::filesystem::exists(p)) return p;
        return {};
    }

    // Otherwise match against the display names of installed runners.
    for (const auto& r : scan_proton_runners()) {
        if (r.name == name) return r.binary;
    }
    return {};
}

std::string LinuxPlatform::vdf_value_for_key(const std::string& line, const std::string& key) {
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

std::string LinuxPlatform::read_steam_compat_tool(uint32_t appid) const {
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
                // Look for "name" key - this is the tool name
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

std::filesystem::path LinuxPlatform::resolve_tool_dir(const std::string& tool_name) const {
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

std::filesystem::path LinuxPlatform::find_proton_for_game(uint32_t appid) const {
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
    return find_proton();
}

std::filesystem::path LinuxPlatform::resolve_proton_prefix(uint32_t steam_appid) const {
    auto steam_root = find_steam_root();
    if (steam_root.empty()) return {};
    if (steam_appid == 0) return {};
    return steam_root / "steamapps" / "compatdata" / std::to_string(steam_appid);
}

std::vector<std::filesystem::path> LinuxPlatform::steam_library_paths() const {
    auto steam_root = find_steam_root();
    if (steam_root.empty()) return {};

    std::vector<std::filesystem::path> paths;
    auto lib_folders = steam_root / "steamapps" / "libraryfolders.vdf";
    if (std::filesystem::exists(lib_folders)) {
        std::ifstream lf(lib_folders);
        std::string line;
        while (std::getline(lf, line)) {
            auto val = vdf_value_for_key(line, "path");
            if (!val.empty()) {
                paths.emplace_back(val);
            }
        }
    }
    if (paths.empty()) {
        paths.push_back(steam_root);
    }
    return paths;
}

std::filesystem::path LinuxPlatform::prefix_user_dir(const std::filesystem::path& prefix) {
    if (prefix.empty()) return {};

    // Proton prefixes store drive_c under a `pfx` subdirectory
    // (compatdata/<appid>/pfx/drive_c/users/<user>).
    auto users_dir = prefix / "drive_c" / "users";
    if (!std::filesystem::exists(users_dir)) {
        users_dir = prefix / "pfx" / "drive_c" / "users";
    }
    if (!std::filesystem::exists(users_dir)) return {};

    // Proton prefixes use "steamuser" by default. Prefer it when present,
    // otherwise pick the first non-system user directory.
    std::filesystem::path steamuser = users_dir / "steamuser";
    if (std::filesystem::is_directory(steamuser)) return steamuser;

    static const std::vector<std::string> system_users = {
        "Public", "Default", "Default User", "All Users",
    };

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(users_dir, ec)) {
        if (!entry.is_directory()) continue;
        auto name = entry.path().filename().string();
        if (std::find(system_users.begin(), system_users.end(), name) != system_users.end())
            continue;
        if (name == "steamuser") return entry.path();
        if (steamuser.empty()) steamuser = entry.path();
    }
    return steamuser;
}

std::filesystem::path LinuxPlatform::game_documents_dir(uint32_t steam_appid) const {
    auto user = prefix_user_dir(resolve_proton_prefix(steam_appid));
    if (user.empty()) return {};
    return user / "Documents";
}

std::filesystem::path LinuxPlatform::game_local_appdata_dir(uint32_t steam_appid) const {
    auto user = prefix_user_dir(resolve_proton_prefix(steam_appid));
    if (user.empty()) return {};
    return user / "AppData" / "Local";
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

// --- Home / temp / thread priority ---

std::filesystem::path LinuxPlatform::home_dir() const {
    if (const char* home = std::getenv("HOME"); home && home[0] != '\0')
        return std::filesystem::path(home);
    return std::filesystem::temp_directory_path();
}

std::filesystem::path LinuxPlatform::temp_dir() const {
    return std::filesystem::temp_directory_path();
}

void LinuxPlatform::set_thread_low_priority() const {
    setpriority(PRIO_PROCESS, 0, 10);
}

// --- XDG mimeapps helpers (KDE-aware) ---
//
// GIO (used by xdg-desktop-portal) and KService both resolve a scheme's
// default app from the desktop-specific mimeapps file first
// (kde-mimeapps.list under XDG_CURRENT_DESKTOP=KDE), then the generic ones.
// `xdg-mime default` only writes the generic config file, which KDE's
// resolution path downgrades -- so registration must propagate the default to
// every file the resolution path actually consults.

namespace {

struct MimeEntry {
    std::string key;
    std::string value;
};

using MimeGroup = std::pair<std::string, std::vector<MimeEntry>>;

std::filesystem::path xdg_dir_or(const char* env_var,
                                 const std::filesystem::path& fallback) {
    if (const char* val = std::getenv(env_var); val && val[0] != '\0') {
        return std::filesystem::path(val);
    }
    return fallback;
}

bool read_mimeapps_groups(const std::filesystem::path& path,
                          std::vector<MimeGroup>& groups) {
    std::ifstream f(path);
    if (!f) return false;
    groups.clear();
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            groups.emplace_back(line.substr(1, line.size() - 2),
                                std::vector<MimeEntry>{});
            continue;
        }
        if (groups.empty()) continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto& entries = groups.back().second;
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        bool replaced = false;
        for (auto& e : entries) {
            if (e.key == key) {
                e.value = value;
                replaced = true;
                break;
            }
        }
        if (!replaced) entries.push_back({key, value});
    }
    return true;
}

void write_mimeapps_groups(const std::filesystem::path& path,
                           const std::vector<MimeGroup>& groups) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream f(path);
    if (!f) return;
    for (const auto& [name, entries] : groups) {
        f << '[' << name << "]\n";
        for (const auto& e : entries) {
            f << e.key << '=' << e.value << '\n';
        }
    }
}

void set_entry(std::vector<MimeEntry>& entries, const std::string& key,
               const std::string& value) {
    for (auto& e : entries) {
        if (e.key == key) {
            e.value = value;
            return;
        }
    }
    entries.push_back({key, value});
}

void remove_entry(std::vector<MimeEntry>& entries, const std::string& key) {
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [&](const MimeEntry& e) { return e.key == key; }),
                  entries.end());
}

// Sets/updates the default app for `scheme` in one mimeapps file.
// Desktop-specific files (e.g. kde-mimeapps.list) may only override defaults;
// the spec-invalid association entry is dropped there so GIO stops warning.
void set_mimeapps_scheme(const std::filesystem::path& path,
                         const std::string& scheme,
                         const std::string& desktop_id,
                         bool desktop_specific) {
    std::vector<MimeGroup> groups;
    read_mimeapps_groups(path, groups);

    bool found_defaults = false;
    for (auto& [name, entries] : groups) {
        if (name == "Default Applications") {
            found_defaults = true;
            set_entry(entries, scheme, desktop_id + ".desktop");
        } else if (name == "Added Associations") {
            if (desktop_specific) {
                remove_entry(entries, scheme);
            } else {
                set_entry(entries, scheme, desktop_id + ".desktop;");
            }
        }
    }
    if (!found_defaults) {
        groups.push_back({"Default Applications", {{scheme, desktop_id + ".desktop"}}});
    }

    if (desktop_specific) {
        groups.erase(std::remove_if(groups.begin(), groups.end(),
                                    [](const MimeGroup& g) {
                                        return g.first == "Added Associations" &&
                                               g.second.empty();
                                    }),
                     groups.end());
    }

    write_mimeapps_groups(path, groups);
}

// The mimeapps files that resolve defaults for the current desktop, in spec
// priority order.
std::vector<std::filesystem::path> mimeapps_files() {
    const char* home = std::getenv("HOME");
    std::vector<std::filesystem::path> files;
    if (!home) return files;
    const auto config_home =
        xdg_dir_or("XDG_CONFIG_HOME", std::filesystem::path(home) / ".config");
    const auto data_home =
        xdg_dir_or("XDG_DATA_HOME", std::filesystem::path(home) / ".local" / "share");

    if (const char* desktop = std::getenv("XDG_CURRENT_DESKTOP"); desktop) {
        std::string d = desktop;
        for (auto& c : d) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (d.find("kde") != std::string::npos) {
            files.push_back(config_home / "kde-mimeapps.list");
        }
    }
    files.push_back(config_home / "mimeapps.list");
    files.push_back(data_home / "applications" / "mimeapps.list");
    return files;
}

// Propagates `desktop_id` as the default for `scheme` across all mimeapps
// files the resolution path consults.
void set_mimeapps_defaults(const std::string& scheme, const std::string& desktop_id) {
    const char* home = std::getenv("HOME");
    if (!home) return;
    const auto config_home =
        xdg_dir_or("XDG_CONFIG_HOME", std::filesystem::path(home) / ".config");
    const auto data_home =
        xdg_dir_or("XDG_DATA_HOME", std::filesystem::path(home) / ".local" / "share");

    set_mimeapps_scheme(config_home / "kde-mimeapps.list", scheme, desktop_id, true);
    set_mimeapps_scheme(config_home / "mimeapps.list", scheme, desktop_id, false);
    const auto data_file = data_home / "applications" / "mimeapps.list";
    if (std::filesystem::exists(data_file)) {
        set_mimeapps_scheme(data_file, scheme, desktop_id, false);
    }
}

// The effective default app for `scheme` per the mimeapps files, or empty.
std::string default_for_scheme(const std::string& scheme) {
    for (const auto& path : mimeapps_files()) {
        std::vector<MimeGroup> groups;
        if (!read_mimeapps_groups(path, groups)) continue;
        for (const auto& [name, entries] : groups) {
            if (name != "Default Applications") continue;
            for (const auto& e : entries) {
                if (e.key == scheme) return e.value;
            }
        }
    }
    return {};
}

}  // namespace

// --- NXM protocol handler registration (XDG) ---

static const char* NXM_DESKTOP_FILE = "gamemodmanager-nxm.desktop";
static const char* NXM_DESKTOP_ID = "gamemodmanager-nxm";

static std::filesystem::path nxm_desktop_path() {
    auto home = std::getenv("HOME");
    if (!home) return {};
    return std::filesystem::path(home) / ".local" / "share" / "applications" / NXM_DESKTOP_FILE;
}

bool LinuxPlatform::register_nxm_handler(const std::filesystem::path& exe_path) {
    auto desktop = nxm_desktop_path();
    if (desktop.empty()) return false;

    std::error_code ec;
    std::filesystem::create_directories(desktop.parent_path(), ec);

    std::ofstream f(desktop);
    if (!f) return false;

    // Quote the Exec path only when it contains whitespace. xdg-mime's
    // desktop_file_to_binary() runs `command -v` on the raw first word and does
    // NOT strip quotes, so a quoted path makes `xdg-mime query default` skip
    // this entry entirely and is_nxm_handler_registered() reports false even
    // when we are the default.
    const std::string exe = exe_path.string();
    const bool quote = exe.find_first_of(" \t") != std::string::npos;

    f << "[Desktop Entry]\n"
      << "Type=Application\n"
      << "Name=GameModManager\n"
      << "Comment=Download Nexus Mods via nxm:// links\n"
      << "Exec=" << (quote ? "\"" : "") << exe << (quote ? "\"" : "") << " --handle-nxm %u\n"
      << "Terminal=false\n"
      << "MimeType=x-scheme-handler/nxm;\n"
      << "NoDisplay=true\n"
      << "Categories=Game;\n";

    f.close();
    if (!f.good()) return false;

    // Register with xdg-mime as the default handler (generic config file;
    // keeps non-KDE desktops correct)
    std::string cmd = "xdg-mime default " + std::string(NXM_DESKTOP_ID) + ".desktop x-scheme-handler/nxm 2>/dev/null";
    std::system(cmd.c_str());

    // KDE's resolution (GIO + KService/KSycoca) reads kde-mimeapps.list and
    // the data-level mimeapps file before/along the generic one, so propagate
    // the default to every file the resolution path consults.
    set_mimeapps_defaults("x-scheme-handler/nxm", NXM_DESKTOP_ID);

    // Refresh the KService database so portals/choosers pick the change up
    // without a logout. Best-effort.
    std::system("kbuildsycoca6 --noincremental 2>/dev/null");

    return true;
}

bool LinuxPlatform::unregister_nxm_handler() {
    auto desktop = nxm_desktop_path();
    if (!desktop.empty()) {
        std::error_code ec;
        std::filesystem::remove(desktop, ec);
    }

    // Reset to the previous default (or remove)
    std::string cmd = "xdg-mime default org.gnome.Nautilus.desktop x-scheme-handler/nxm 2>/dev/null";
    // Note: this is a best-effort reset; the user may have had a different handler
    // A more correct approach would be to read the old value before registering
    std::system(cmd.c_str());
    return true;
}

bool LinuxPlatform::is_nxm_handler_registered() {
    auto desktop = nxm_desktop_path();
    if (!std::filesystem::exists(desktop)) return false;

    // KDE-aware: read the mimeapps files in spec priority order. `xdg-mime
    // query default` is not used here because its desktop_file_to_binary()
    // skips entries whose Exec cannot be resolved (e.g. amethyst's quoted
    // path), which makes it report us as the default even when the KDE state
    // still names a stale competitor.
    const std::string value = default_for_scheme("x-scheme-handler/nxm");
    if (value.empty()) return false;
    const auto first = value.substr(0, value.find(';'));
    return first == (std::string(NXM_DESKTOP_ID) + ".desktop");
}

// --- GMM protocol handler registration (XDG) ---

static const char* GMM_DESKTOP_FILE = "gamemodmanager-gmm.desktop";
static const char* GMM_DESKTOP_ID = "gamemodmanager-gmm";

static std::filesystem::path gmm_desktop_path() {
    auto home = std::getenv("HOME");
    if (!home) return {};
    return std::filesystem::path(home) / ".local" / "share" / "applications" / GMM_DESKTOP_FILE;
}

bool LinuxPlatform::register_gmm_handler(const std::filesystem::path& exe_path) {
    auto desktop = gmm_desktop_path();
    if (desktop.empty()) return false;

    std::error_code ec;
    std::filesystem::create_directories(desktop.parent_path(), ec);

    std::ofstream f(desktop);
    if (!f) return false;

    // Same quoting rule as register_nxm_handler: keep the path unquoted unless
    // it contains whitespace so xdg-mime can resolve this entry.
    const std::string exe = exe_path.string();
    const bool quote = exe.find_first_of(" \t") != std::string::npos;

    f << "[Desktop Entry]\n"
      << "Type=Application\n"
      << "Name=GameModManager (gmm://)\n"
      << "Comment=Download mods via gmm:// links\n"
      << "Exec=" << (quote ? "\"" : "") << exe << (quote ? "\"" : "") << " --handle-gmm %u\n"
      << "Terminal=false\n"
      << "MimeType=x-scheme-handler/gmm;\n"
      << "NoDisplay=true\n"
      << "Categories=Game;\n";

    f.close();
    if (!f.good()) return false;

    std::string cmd = "xdg-mime default " + std::string(GMM_DESKTOP_ID) + ".desktop x-scheme-handler/gmm 2>/dev/null";
    std::system(cmd.c_str());

    // Same KDE-aware propagation as register_nxm_handler.
    set_mimeapps_defaults("x-scheme-handler/gmm", GMM_DESKTOP_ID);
    std::system("kbuildsycoca6 --noincremental 2>/dev/null");

    return true;
}

bool LinuxPlatform::unregister_gmm_handler() {
    auto desktop = gmm_desktop_path();
    if (!desktop.empty()) {
        std::error_code ec;
        std::filesystem::remove(desktop, ec);
    }

    std::string cmd = "xdg-mime default org.gnome.Nautilus.desktop x-scheme-handler/gmm 2>/dev/null";
    std::system(cmd.c_str());
    return true;
}

bool LinuxPlatform::is_gmm_handler_registered() {
    auto desktop = gmm_desktop_path();
    if (!std::filesystem::exists(desktop)) return false;

    // KDE-aware, same rationale as is_nxm_handler_registered.
    const std::string value = default_for_scheme("x-scheme-handler/gmm");
    if (value.empty()) return false;
    const auto first = value.substr(0, value.find(';'));
    return first == (std::string(GMM_DESKTOP_ID) + ".desktop");
}

}  // namespace engine
