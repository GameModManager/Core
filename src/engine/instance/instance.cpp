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
        InstanceKind::Mods, InstanceKind::Profiles, InstanceKind::Downloads,
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
    return out.good();
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
