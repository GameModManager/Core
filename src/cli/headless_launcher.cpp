#include "cli/headless_launcher.h"

#include "engine/launcher.h"
#include "engine/log/logger.h"

#include <chrono>
#include <filesystem>
#include <thread>
#include <unistd.h>
#include <sys/wait.h>

namespace fs = std::filesystem;

namespace cli {

int launch_game_headless(const HeadlessConfig& cfg) {
    engine::Logger::instance().enable_console();
    engine::Logger::instance().debug("GameModManager - headless launch");
    engine::Logger::instance().debug(
        "  game_dir: " + cfg.game_dir.string());
    engine::Logger::instance().debug(
        "  overwrite: " + cfg.overwrite_dir.string());
    engine::Logger::instance().debug(
        "  appid: " + std::to_string(cfg.steam_appid) +
        "  windows: " + (cfg.is_windows_exe ? "yes" : "no"));

    if (!fs::exists(cfg.executable)) {
        engine::Logger::instance().error(
            "Executable not found: " + cfg.executable.string());
        return 2;
    }

    engine::LaunchParams lparams;
    lparams.executable = cfg.executable;
    lparams.game_dir = cfg.game_dir;
    lparams.overwrite_dir = cfg.overwrite_dir;
    lparams.steam_appid = cfg.steam_appid;
    lparams.is_windows_exe = cfg.is_windows_exe;

    auto launch_time = fs::file_time_type::clock::now();
    auto result = engine::launch_game(lparams);

    if (result.pid <= 0) {
        engine::Logger::instance().error("Headless: failed to launch game");
        return 3;
    }

    engine::Logger::instance().info(
        "Headless: game launched (pid=" + std::to_string(result.pid) +
        "). Waiting for exit...");

    // Wait for the game process to exit. For ProtonRuntime this blocks on
    // the Proton script (waitforexitandrun) which stays alive for the full
    // game duration. For NativeRuntime it blocks on the game process directly.
    // PGID-based polling is unreliable here because Proton creates child
    // sessions with different PGIDs internally.
    int status;
    pid_t child = static_cast<pid_t>(result.pid);
    pid_t ret;
    do {
        ret = waitpid(child, &status, 0);
    } while (ret == -1 && errno == EINTR);

    engine::Logger::instance().info(
        "Headless: game exited, waiting 3s for delayed writes");
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // Post-hoc capture (no-op if overlay was used)
    engine::capture_overwrite(cfg.game_dir, cfg.overwrite_dir, launch_time);

    engine::Logger::instance().info("Headless: done");
    return (ret > 0 && WIFEXITED(status)) ? WEXITSTATUS(status) : 1;
}

}
