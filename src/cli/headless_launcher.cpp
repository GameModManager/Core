#include "cli/headless_launcher.h"

#include "engine/core/instance/instance_utils.h"
#include "engine/deploy/launch/launcher.h"
#include "engine/core/log/logger.h"
#include "engine/game/detect/mod_scanner.h"
#include "engine/game/plugins/plugin_database.h"
#include "engine/game/registry/game_knowledge.h"
#include "engine/profile/profile.h"

#include <chrono>
#include <filesystem>
#include <thread>
#ifndef _WIN32
#include <unistd.h>
#include <sys/wait.h>
#endif

#ifdef _WIN32
using pid_t = int;
#endif

namespace fs = std::filesystem;

namespace cli {

namespace {

// Delayed disable (plugin-declared capability, e.g. Isaac's Direct mode): the
// GUI defers disable-sentinel writes until Run and flushes its in-memory
// deferred queue before the deploy worker starts (LaunchController::
// flush_deferred_disable_queue). The CLI has no queue - the active profile's
// modlist.txt is the per-profile source of truth, so reconcile the on-disk
// sentinels against it directly. Idempotent (writing an existing sentinel /
// removing an absent one is a no-op) and a no-op for games that do not
// declare delayed_disable.
void reconcile_deferred_disable_sentinels(const HeadlessLauncher::Config& cfg) {
    if (!cfg.knowledge || cfg.game_id.empty())
        return;
    if (!engine::delayed_disable_for(*cfg.knowledge, cfg.game_id))
        return;

    // Resolve the instance's profiles dir (honors the instance.toml override).
    engine::Instance inst = engine::Instance::from_root(cfg.instance_root);
    const auto profiles_dir = inst.path_for(engine::InstanceKind::Profiles);
    if (!fs::is_directory(profiles_dir))
        return;

    // Active profile: "Default" when present, else the first profile dir
    // (mirrors the GUI's refresh_profiles fallback chain; the CLI has no
    // "current profile" state).
    fs::path profile_dir;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(profiles_dir, ec)) {
        if (!entry.is_directory())
            continue;
        if (entry.path().filename() == "Default") {
            profile_dir = entry.path();
            break;
        }
        if (profile_dir.empty())
            profile_dir = entry.path();
    }
    if (profile_dir.empty()) {
        engine::Logger::instance().warn(
            "Delayed disable: no profile found under " + profiles_dir.string() +
            ", skipping sentinel reconciliation");
        return;
    }

    // Read the profile's modlist.txt (the per-profile source of truth for
    // enabled state). refresh_mod_status({}) loads exactly the file entries -
    // no known_mods to append.
    engine::profile::ProfileManager profile(profile_dir);
    profile.refresh_mod_status({});
    const auto mods = profile.mods();
    if (mods.empty())
        return;

    const auto mods_dir = inst.path_for(engine::InstanceKind::Mods);
    // Game-native mods dir (Workspace-otx chain): instance.toml override >
    // plugin-declared "game_mods_dir" hook > game_dir/mods_subpath.
    std::string inst_mods_override;
    if (inst.read_toml())
        inst_mods_override = inst.info().game_mods_dir.string();
    const auto native_mods_dir = engine::resolve_game_mods_dir(
        cfg.game_id, cfg.game_dir, *cfg.knowledge, inst_mods_override);

    int reconciled = 0;
    for (const auto& m : mods) {
        if (m.foreign)
            continue;  // unmanaged (DLC etc.) - never written as +/- toggle
        auto folder = mods_dir / m.mod_id;
        if (!fs::exists(folder)) {
            auto fallback = native_mods_dir / m.mod_id;
            if (fs::exists(fallback))
                folder = fallback;
        }
        if (m.enabled) {
            (void)engine::ModScanner::enable_mod(*cfg.knowledge, cfg.game_id, folder);
        } else {
            (void)engine::ModScanner::disable_mod(*cfg.knowledge, cfg.game_id, folder);
        }
        ++reconciled;
    }
    engine::Logger::instance().debug(
        "Delayed disable: reconciled " + std::to_string(reconciled) +
        " sentinel(s) from profile '" + profile_dir.filename().string() +
        "' before launch");
}

}  // namespace

HeadlessLauncher::HeadlessLauncher(const Config& config,
                                   engine::Platform* platform)
    : config_(config), platform_(platform) {}

int HeadlessLauncher::run() {
    engine::Logger::instance().enable_console();
    engine::Logger::instance().debug("GameModManager - headless launch");
    engine::Logger::instance().debug(
        "  game_dir: " + config_.game_dir.string());
    engine::Logger::instance().debug(
        "  instance_root: " + config_.instance_root.string());
    engine::Logger::instance().debug(
        "  appid: " + std::to_string(config_.steam_appid) +
        "  windows: " + (config_.is_windows_exe ? "yes" : "no"));

    // No pre-check here: the executable may only exist in the merged view
    // (deployed into .gmm_staging). prepare_launch_params populates staging;
    // do_launch then validates reachability and fails with a log line.

    // Build launch params through the shared workflow (same as GUI "Run" path)
    engine::LaunchPrepRequest req;
    req.instance_root = config_.instance_root;
    req.game_dir = config_.game_dir;
    req.executable = config_.executable;
    req.knowledge = config_.knowledge ? *config_.knowledge : engine::GameKnowledge();
    req.game_id = config_.game_id;
    req.steam_appid = config_.steam_appid;
    req.is_windows_exe = config_.is_windows_exe;
    req.local_saves_enabled = config_.local_saves_enabled;
    req.platform = platform_;
    auto lparams = engine::prepare_launch_params(req);
    lparams.platform = platform_;

    // MO2-equivalent plugin order: build + write the game's Plugins.txt (and
    // the instance profile) right before launch. No-op for games without
    // plugin support (no localappdata_folder hook).
    engine::PluginDb::Database::write_plugins_txt_for_launch(
        config_.game_dir, config_.instance_root, config_.game_id, config_.steam_appid,
        config_.knowledge ? *config_.knowledge : engine::GameKnowledge(), platform_);

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

    // Post-hoc capture. Only for overlay-capable sessions: an OverlayFS launch
    // already wrote into the upperdir (capture is a no-op), and the plain
    // fallback needs the harvest. Direct-symlink games write straight into
    // game_dir and must NOT be harvested - capture would move the game's own
    // files out of game_dir into Overwrite.
    if (lparams.use_overlay) {
        bool case_insensitive =
            config_.knowledge &&
            config_.knowledge->get(config_.game_id, "case_sensitive", "true") == "false";
        engine::capture_overwrite(config_.game_dir, lparams.overwrite_dir, launch_time,
                                  case_insensitive);
    }

    engine::Logger::instance().debug("Headless: done");
    return (ret > 0 && WIFEXITED(status)) ? WEXITSTATUS(status) : 1;
}

}
