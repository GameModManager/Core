#include "cli/headless_launcher.h"

#include "engine/instance/instance_utils.h"
#include "engine/launcher.h"
#include "engine/log/logger.h"
#include "engine/plugins/plugin_database.h"
#include "engine/registry/game_knowledge.h"

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
        "  instance_root: " + cfg.instance_root.string());
    engine::Logger::instance().debug(
        "  appid: " + std::to_string(cfg.steam_appid) +
        "  windows: " + (cfg.is_windows_exe ? "yes" : "no"));

    // No pre-check here: the executable may only exist in the merged view
    // (deployed into .gmm_staging). prepare_launch_params populates staging;
    // do_launch then validates reachability and fails with a log line.

    // Build launch params through the shared workflow (same as GUI "Run" path)
    auto lparams = engine::prepare_launch_params(
        cfg.instance_root,
        cfg.game_dir,
        cfg.executable,
        cfg.knowledge ? *cfg.knowledge : engine::GameKnowledge(),
        cfg.game_id,
        cfg.steam_appid,
        cfg.is_windows_exe);
    lparams.platform = cfg.platform;

    // MO2-equivalent plugin order: build + write the game's Plugins.txt (and
    // the instance profile) right before launch. No-op for games without
    // plugin support (no localappdata_folder hook).
    engine::PluginDatabase::write_plugins_txt_for_launch(
        cfg.game_dir, cfg.instance_root, cfg.game_id, cfg.steam_appid,
        cfg.knowledge ? *cfg.knowledge : engine::GameKnowledge(), cfg.platform);

    auto launch_time = fs::file_time_type::clock::now();
    auto result = engine::launch_game(lparams);

    if (result.pid <= 0) {
        engine::Logger::instance().error("Headless: failed to launch game");
        return 3;
    }

    engine::Logger::instance().info(
        "Headless: game launched (pid=" + std::to_string(result.pid) +
        "). Waiting for exit...");

    int status;
    pid_t child = static_cast<pid_t>(result.pid);
    pid_t ret;
    do {
        ret = waitpid(child, &status, 0);
    } while (ret == -1 && errno == EINTR);

    // Remove the launch cgroup now that every process has exited
    // (best-effort; empty cgroup v2 dirs otherwise accumulate).
    engine::cgroup_remove({result.cgroup_path});

    engine::Logger::instance().info(
        "Headless: game exited, waiting 3s for delayed writes");
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // Post-hoc capture (no-op if overlay was used)
    engine::capture_overwrite(cfg.game_dir, lparams.overwrite_dir, launch_time);

    engine::Logger::instance().debug("Headless: done");
    return (ret > 0 && WIFEXITED(status)) ? WEXITSTATUS(status) : 1;
}

}
