#include "engine/instance/instance_utils.h"
#include "engine/log/logger.h"

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

}
