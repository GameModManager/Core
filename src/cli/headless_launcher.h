#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace cli {

struct HeadlessConfig {
    std::filesystem::path executable;
    std::filesystem::path game_dir;
    std::filesystem::path overwrite_dir;
    std::filesystem::path instance_root;
    uint32_t steam_appid = 0;
    bool is_windows_exe = false;
};

int launch_game_headless(const HeadlessConfig& cfg);

}
