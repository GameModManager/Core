#include "engine/game/detect/game_detector.h"

#include <algorithm>
#include <fstream>
#include <optional>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace engine {

namespace {

std::vector<std::filesystem::path> default_steam_roots() {
    std::vector<std::filesystem::path> roots;

#ifdef _WIN32
    // Windows: try registry first
    HKEY hkey;
    LONG result = RegOpenKeyExW(
        HKEY_CURRENT_USER, L"SOFTWARE\\Valve\\Steam", 0, KEY_READ, &hkey);
    if (result == ERROR_SUCCESS) {
        wchar_t buf[MAX_PATH];
        DWORD buf_size = sizeof(buf);
        DWORD type = REG_SZ;
        result = RegQueryValueExW(hkey, L"SteamPath", nullptr, &type,
                                  reinterpret_cast<LPBYTE>(buf), &buf_size);
        RegCloseKey(hkey);
        if (result == ERROR_SUCCESS && type == REG_SZ) {
            std::wstring ws(buf, buf_size / sizeof(wchar_t));
            // Registry uses forward slashes; normalize
            for (auto& c : ws) {
                if (c == L'/') c = L'\\';
            }
            auto root = std::filesystem::path(ws);
            roots.push_back(root);
        }
    }
    // Common Windows Steam paths
    roots.push_back(LR"(C:\Program Files (x86)\Steam)");
    roots.push_back(LR"(C:\Program Files\Steam)");
#else
    // Linux / macOS
    auto home = std::getenv("HOME");
    if (!home) return roots;

    std::filesystem::path home_path(home);
    roots.push_back(home_path / ".local" / "share" / "Steam");
    roots.push_back(home_path / ".steam" / "steam");
    roots.push_back(home_path / ".steam" / "debian-installation");
#endif

    return roots;
}

std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n\"");
    auto end = s.find_last_not_of(" \t\r\n\"");
    if (start == std::string::npos) return {};
    return s.substr(start, end - start + 1);
}

}  // namespace

std::optional<GameDetector::SteamPaths> GameDetector::find_steam() {
    for (const auto& root : default_steam_roots()) {
        auto vdf = root / "steamapps" / "libraryfolders.vdf";
        if (std::filesystem::exists(vdf)) {
            SteamPaths paths;
            paths.steam_root = root;
            paths.library_folders = parse_library_folders(vdf);
            if (paths.library_folders.empty()) {
                paths.library_folders.push_back(root / "steamapps");
            }
            return paths;
        }
    }
    return std::nullopt;
}

std::vector<std::filesystem::path> GameDetector::parse_library_folders(
    const std::filesystem::path& libraryfolders_vdf) {
    std::ifstream file(libraryfolders_vdf);
    if (!file) return {};

    std::vector<std::filesystem::path> result;
    std::string line;

    while (std::getline(file, line)) {
        auto pos = line.find("\"path\"");
        if (pos == std::string::npos) continue;

        auto value_start = line.find('"', pos + 6);
        if (value_start == std::string::npos) continue;
        value_start++;

        auto value_end = line.find('"', value_start);
        if (value_end == std::string::npos) continue;

        std::string path_str = line.substr(value_start, value_end - value_start);
        auto lib_path = std::filesystem::path(trim(path_str)) / "steamapps";
        if (std::filesystem::exists(lib_path)) {
            result.push_back(lib_path);
        }
    }

    return result;
}

std::optional<std::filesystem::path> GameDetector::find_app_install(
    const std::filesystem::path& library_folder, uint32_t appid) {
    auto acf = library_folder / ("appmanifest_" + std::to_string(appid) + ".acf");
    if (!std::filesystem::exists(acf)) return std::nullopt;

    std::ifstream file(acf);
    if (!file) return std::nullopt;

    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());

    auto installdir = parse_acf_value(content, "installdir");
    if (!installdir || installdir->empty()) return std::nullopt;

    return library_folder / "common" / *installdir;
}

std::optional<std::string> GameDetector::parse_acf_value(
    const std::string& acf_content, const std::string& key) {
    std::istringstream stream(acf_content);
    std::string line;

    while (std::getline(stream, line)) {
        auto key_pos = line.find('"' + key + '"');
        if (key_pos == std::string::npos) continue;

        auto eq_pos = line.find('\t', key_pos);
        if (eq_pos == std::string::npos) continue;

        auto val_start = line.find('"', eq_pos);
        if (val_start == std::string::npos) continue;
        val_start++;

        auto val_end = line.find('"', val_start);
        if (val_end == std::string::npos) continue;

        return line.substr(val_start, val_end - val_start);
    }
    return std::nullopt;
}

std::vector<DetectedGame> GameDetector::detect_steam_games(
    uint32_t steam_appid, const std::string& game_id, const std::string& game_name) {
    return detect_steam_games_multi({{steam_appid, {game_id, game_name}}});
}

std::vector<DetectedGame> GameDetector::detect_steam_games_multi(
    const std::vector<std::pair<uint32_t, std::pair<std::string, std::string>>>& games) {
    std::vector<DetectedGame> results;

    auto steam = find_steam();
    if (!steam) return results;

    for (const auto& [appid, info] : games) {
        const auto& [game_id, game_name] = info;

        for (const auto& lib : steam->library_folders) {
            auto path = find_app_install(lib, appid);
            if (path) {
                DetectedGame detected;
                detected.game_id = game_id;
                detected.name = game_name;
                detected.steam_appid = appid;
                detected.install_path = *path;
                results.push_back(std::move(detected));
                break;
            }
        }
    }

    return results;
}

}  // namespace engine
