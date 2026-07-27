#include "engine/instance/instance.h"

#include <fstream>
#include <sstream>

namespace engine {

Instance Instance::portable(const std::filesystem::path& root) {
    Instance inst;
    inst.info_.root = root;
    inst.info_.portable = true;
    return inst;
}

Instance Instance::installed(const std::string& name,
                            const std::filesystem::path& instances_root) {
    Instance inst;
    inst.info_.root = instances_root / name;
    inst.info_.portable = false;
    return inst;
}

std::filesystem::path Instance::path_for(InstanceKind kind) const {
    switch (kind) {
        case InstanceKind::Mods:             return info_.root / "mods";
        case InstanceKind::Profiles:         return info_.root / "profiles";
        case InstanceKind::Downloads:        return info_.root / "downloads";
        case InstanceKind::Cache:            return info_.root / "cache";
        case InstanceKind::CacheArchives:    return info_.root / "cache" / "archives";
        case InstanceKind::CacheThumbnails:  return info_.root / "cache" / "thumbnails";
        case InstanceKind::Plugins:          return info_.root / "plugins";
        case InstanceKind::Logs:             return info_.root / "logs";
        case InstanceKind::Config:           return info_.root / "config";
        case InstanceKind::Overwrite:        return info_.root / "mods" / "Overwrite";
    }
    return {};
}

std::filesystem::path Instance::toml_path() const {
    return info_.root / "instance.toml";
}

bool Instance::create_directories() const {
    std::error_code ec;
    std::filesystem::create_directories(info_.root, ec);
    if (ec) return false;

    const InstanceKind dirs[] = {
        InstanceKind::Mods, InstanceKind::Overwrite, InstanceKind::Profiles,
        InstanceKind::Downloads,
        InstanceKind::CacheArchives, InstanceKind::CacheThumbnails,
        InstanceKind::Plugins, InstanceKind::Logs, InstanceKind::Config,
    };
    for (auto kind : dirs) {
        std::filesystem::create_directories(path_for(kind), ec);
        if (ec) return false;
    }
    return true;
}

bool Instance::write_toml() const {
    std::ofstream out(toml_path());
    if (!out) return false;

    out << "game_id = \"" << info_.game_id << "\"\n";
    out << "portable = " << (info_.portable ? "true" : "false") << "\n";
    if (!info_.game_dir.empty()) {
        out << "game_dir = \"" << info_.game_dir.string() << "\"\n";
    }
    return out.good();
}

bool Instance::read_toml() {
    std::ifstream in(toml_path());
    if (!in) return false;

    std::string line;
    while (std::getline(in, line)) {
        // game_id
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto key = line.substr(0, eq);
        // trim whitespace
        key.erase(key.find_last_not_of(" \t") + 1);
        key.erase(0, key.find_first_not_of(" \t"));

        auto val_start = line.find('"', eq + 1);
        if (val_start == std::string::npos) continue;
        auto val_end = line.find('"', val_start + 1);
        if (val_end == std::string::npos) continue;
        auto val = line.substr(val_start + 1, val_end - val_start - 1);

        if (key == "game_id") {
            info_.game_id = val;
        } else if (key == "game_dir") {
            info_.game_dir = val;
        } else if (key == "portable") {
            info_.portable = (val == "true");
        }
    }
    return true;
}

std::filesystem::path Instance::resolve_portable_root(
    const std::filesystem::path& exe_dir) {
    auto toml = exe_dir / "instance.toml";
    if (std::filesystem::exists(toml)) {
        return exe_dir;
    }
    return {};
}

bool Instance::is_portable(const std::filesystem::path& exe_dir) {
    return std::filesystem::exists(exe_dir / "instance.toml");
}

}  // namespace engine
