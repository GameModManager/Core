#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace engine {
class GameKnowledge;
class PlatformInterface;
}

namespace cli {

struct HeadlessConfig {
    std::filesystem::path executable;
    std::filesystem::path game_dir;
    std::filesystem::path instance_root;
    uint32_t steam_appid = 0;
    bool is_windows_exe = false;
    const engine::GameKnowledge* knowledge = nullptr;
    const engine::PlatformInterface* platform = nullptr;
    std::string game_id;
    // Per-profile local saves (MO2 GamebryoLocalSavegames). Mirrors the GUI
    // Settings::local_saves() toggle (default off). Wired through the shared
    // prepare_launch_params path so both CLI and GUI agree.
    bool local_saves_enabled = false;
};

int launch_game_headless(const HeadlessConfig& cfg);

}

