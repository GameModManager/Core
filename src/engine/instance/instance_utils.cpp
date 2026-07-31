#include "engine/instance/instance_utils.h"
#include "engine/deploy/deploy_utils.h"
#include "engine/launcher.h"
#include "engine/log/logger.h"
#include "engine/overlay_launcher.h"
#include "engine/registry/game_knowledge.h"

#include <cstdlib>
#include <fstream>
#include <unordered_set>

namespace fs = std::filesystem;

namespace engine {

fs::path default_instances_dir() {
    const char* home = std::getenv("HOME");
    if (!home) return "/tmp/gamemodmanager/instances";
    return std::string(home) + "/.local/share/GameModManager/instances";
}

fs::path last_instance_file_path() {
    const char* home = std::getenv("HOME");
    if (!home) return "/tmp/gamemodmanager/last_instance";
    return std::string(home) + "/.local/share/GameModManager/last_instance";
}

std::string read_last_instance() {
    std::ifstream f(last_instance_file_path());
    std::string name;
    if (f) std::getline(f, name);
    return name;
}

void write_last_instance(const std::string& name) {
    std::ofstream f(last_instance_file_path());
    if (f) f << name << "\n";
}

std::vector<std::string> scan_instances() {
    std::vector<std::string> result;
    std::error_code ec;
    auto dir = default_instances_dir();
    if (!fs::is_directory(dir, ec)) return result;

    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_directory()) continue;
        auto toml = entry.path() / "instance.toml";
        if (fs::exists(toml)) {
            result.push_back(entry.path().filename().string());
        }
    }
    return result;
}

fs::path resolve_instance_path(const std::string& name_or_path) {
    if (name_or_path.empty()) return {};

    fs::path p(name_or_path);
    if (p.is_absolute()) {
        if (fs::exists(p / "instance.toml"))
            return p;
        return {};
    }

    auto resolved = default_instances_dir() / name_or_path;
    if (fs::exists(resolved / "instance.toml"))
        return resolved;
    return {};
}

Instance create_instance_for_game(const DetectedGame& game,
                                   const fs::path& instances_root) {
    std::string inst_name = Instance::to_instance_name(game.name.empty() ? game.game_id : game.name);
    Instance inst = Instance::installed(inst_name, instances_root);
    inst.info().game_id = game.game_id;
    inst.info().game_dir = game.install_path;
    inst.info().steam_appid = game.steam_appid;

    if (!inst.create_directories()) {
        Logger::instance().error(
            "Failed to create instance directories for " + inst_name);
        return Instance::portable({});
    }
    if (!inst.write_toml()) {
        Logger::instance().error(
            "Failed to write instance.toml for " + inst_name);
        return Instance::portable({});
    }

    Logger::instance().debug(
        "Instance created: " + inst_name + " (game=" + game.game_id +
        ") at " + inst.info().root.string());
    return inst;
}

LaunchParams prepare_launch_params(
    const fs::path& instance_root,
    const fs::path& game_dir,
    const fs::path& executable,
    const GameKnowledge& knowledge,
    const std::string& game_id,
    uint32_t steam_appid,
    bool is_windows_exe)
{
    LaunchParams params;
    params.executable = executable;
    params.game_dir = game_dir;
    params.overwrite_dir = instance_root / "overwrite";
    params.steam_appid = steam_appid;
    params.is_windows_exe = is_windows_exe;

    // Validate inputs
    if (!fs::is_directory(game_dir)) {
        Logger::instance().error("prepare_launch_params: game_dir not found: " + game_dir.string());
        return params;
    }

    // Ensure overwrite dir exists (belt-and-suspenders - instance setup should have created it)
    std::error_code ec;
    fs::create_directories(params.overwrite_dir, ec);

    // Check if OverlayFS is supported for this instance
    if (!OverlayFsLauncher::is_supported(params.overwrite_dir)) {
        Logger::instance().info("OverlayFS not supported, launching without overlay");
        return params;
    }

    // Ensure staging dir exists (fixes ENOENT when no mods have been deployed yet)
    auto staging_dir = instance_root / ".gmm_staging";
    fs::create_directories(staging_dir, ec);
    if (ec) {
        Logger::instance().error("Failed to create staging dir: " + ec.message());
        return params;
    }

    // Deploy all enabled mods to staging
    std::string deploy_prefix = knowledge.get(game_id, "deploy_prefix", "Data");
    std::string deploy_include_mod_id = knowledge.get(game_id, "deploy_include_mod_id", "false");
    std::string disable_mechanism = knowledge.get(game_id, "disable_mechanism", "");
    auto mods_dir = instance_root / "mods";

    bool deployed = deploy_all_enabled_mods(mods_dir, staging_dir, deploy_prefix,
                           deploy_include_mod_id == "true", disable_mechanism);
    if (!deployed) {
        Logger::instance().warn("Some mods failed to deploy to staging - continuing anyway");
    }

    params.extra_lowerdirs.push_back(staging_dir);
    Logger::instance().info("Launch: OverlayFS staging at " + staging_dir.string());
    return params;
}

}
