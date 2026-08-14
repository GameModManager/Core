#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace engine {

struct DetectedGame {
    std::string game_id;
    std::string name;
    uint32_t steam_appid = 0;
    std::filesystem::path install_path;
};

class GameDetector {
public:
    struct SteamPaths {
        std::filesystem::path steam_root;
        std::vector<std::filesystem::path> library_folders;
    };

    static std::vector<DetectedGame> detect_steam_games(
        uint32_t steam_appid,
        const std::string& game_id,
        const std::string& game_name);

    static std::vector<DetectedGame> detect_steam_games_multi(
        const std::vector<std::pair<uint32_t, std::pair<std::string, std::string>>>& games);

private:
    static std::optional<SteamPaths> find_steam();
    static std::vector<std::filesystem::path> parse_library_folders(
        const std::filesystem::path& libraryfolders_vdf);
    static std::optional<std::filesystem::path> find_app_install(
        const std::filesystem::path& library_folder, uint32_t appid);
    static std::optional<std::string> parse_acf_value(
        const std::string& acf_content, const std::string& key);
};

}  // namespace engine
