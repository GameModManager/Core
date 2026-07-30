#include "platform/linux/linux_platform.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
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

    f << "[Desktop Entry]\n"
      << "Type=Application\n"
      << "Name=GameModManager\n"
      << "Comment=Download Nexus Mods via nxm:// links\n"
      << "Exec=\"" << exe_path.string() << "\" --handle-nxm %u\n"
      << "Terminal=false\n"
      << "MimeType=x-scheme-handler/nxm;\n"
      << "NoDisplay=true\n"
      << "Categories=Game;\n";

    f.close();
    if (!f.good()) return false;

    // Register with xdg-mime as the default handler
    std::string cmd = "xdg-mime default " + std::string(NXM_DESKTOP_ID) + ".desktop x-scheme-handler/nxm 2>/dev/null";
    return std::system(cmd.c_str()) == 0;
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

    // Check if xdg-mime thinks we're the default
    FILE* pipe = popen("xdg-mime query default x-scheme-handler/nxm 2>/dev/null", "r");
    if (!pipe) return false;

    char buf[256];
    std::string result;
    while (fgets(buf, sizeof(buf), pipe)) result += buf;
    pclose(pipe);

    // Trim whitespace
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();

    return result == (std::string(NXM_DESKTOP_ID) + ".desktop");
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

    f << "[Desktop Entry]\n"
      << "Type=Application\n"
      << "Name=GameModManager (gmm://)\n"
      << "Comment=Download mods via gmm:// links\n"
      << "Exec=\"" << exe_path.string() << "\" --handle-gmm %u\n"
      << "Terminal=false\n"
      << "MimeType=x-scheme-handler/gmm;\n"
      << "NoDisplay=true\n"
      << "Categories=Game;\n";

    f.close();
    if (!f.good()) return false;

    std::string cmd = "xdg-mime default " + std::string(GMM_DESKTOP_ID) + ".desktop x-scheme-handler/gmm 2>/dev/null";
    return std::system(cmd.c_str()) == 0;
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

    FILE* pipe = popen("xdg-mime query default x-scheme-handler/gmm 2>/dev/null", "r");
    if (!pipe) return false;

    char buf[256];
    std::string result;
    while (fgets(buf, sizeof(buf), pipe)) result += buf;
    pclose(pipe);

    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();

    return result == (std::string(GMM_DESKTOP_ID) + ".desktop");
}

}  // namespace engine
