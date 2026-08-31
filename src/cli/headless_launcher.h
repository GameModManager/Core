#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace engine {
class GameKnowledge;
class Platform;
}

namespace cli {

class HeadlessLauncher {
public:
    struct Config {
        std::filesystem::path executable;
        std::filesystem::path game_dir;
        std::filesystem::path instance_root;
        uint32_t steam_appid = 0;
        bool is_windows_exe = false;
        const engine::GameKnowledge* knowledge = nullptr;
        std::string game_id;
        // Per-profile local saves (MO2 GamebryoLocalSavegames). Mirrors the GUI
        // Settings::local_saves() toggle (default off). Wired through the shared
        // prepare_launch_params path so both CLI and GUI agree.
        bool local_saves_enabled = false;
    };

    explicit HeadlessLauncher(const Config& config,
                              engine::Platform* platform = nullptr);
    int run();

private:
    Config config_;
    engine::Platform* platform_;
};

}
