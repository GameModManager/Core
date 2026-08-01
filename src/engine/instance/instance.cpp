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

Instance Instance::from_root(const std::filesystem::path& root) {
    Instance inst;
    inst.info_.root = root;
    return inst;
}

std::filesystem::path Instance::path_for(InstanceKind kind) const {
    switch (kind) {
        case InstanceKind::Mods:
            return info_.mods_dir.empty() ? info_.root / "mods" : info_.mods_dir;
        case InstanceKind::Downloads:
            return info_.downloads_dir.empty() ? info_.root / "downloads" : info_.downloads_dir;
        case InstanceKind::Cache:
            return info_.cache_dir.empty() ? info_.root / "cache" : info_.cache_dir;
        case InstanceKind::CacheArchives: {
            auto base = info_.cache_dir.empty() ? info_.root / "cache" : info_.cache_dir;
            return base / "archives";
        }
        case InstanceKind::CacheThumbnails: {
            auto base = info_.cache_dir.empty() ? info_.root / "cache" : info_.cache_dir;
            return base / "thumbnails";
        }
        case InstanceKind::Profiles:
            return info_.profiles_dir.empty() ? info_.root / "profiles" : info_.profiles_dir;
        case InstanceKind::Overwrite:
            return info_.overwrite_dir.empty() ? info_.root / "overwrite" : info_.overwrite_dir;
        case InstanceKind::Plugins:          return info_.root / "plugins";
        case InstanceKind::Logs:             return info_.root / "logs";
        case InstanceKind::Config:           return info_.root / "config";
        case InstanceKind::Meta:             return info_.root / "meta";
    }
    return {};
}

void Instance::set_path_override(InstanceKind kind, const std::filesystem::path& path) {
    switch (kind) {
        case InstanceKind::Mods:      info_.mods_dir = path; break;
        case InstanceKind::Downloads: info_.downloads_dir = path; break;
        case InstanceKind::Cache:     info_.cache_dir = path; break;
        case InstanceKind::Profiles:  info_.profiles_dir = path; break;
        case InstanceKind::Overwrite: info_.overwrite_dir = path; break;
        default: break;
    }
}

std::filesystem::path Instance::path_override(InstanceKind kind) const {
    switch (kind) {
        case InstanceKind::Mods:      return info_.mods_dir;
        case InstanceKind::Downloads: return info_.downloads_dir;
        case InstanceKind::Cache:     return info_.cache_dir;
        case InstanceKind::Profiles:  return info_.profiles_dir;
        case InstanceKind::Overwrite: return info_.overwrite_dir;
        default:                      return {};
    }
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
        InstanceKind::Meta,
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
    if (info_.steam_appid > 0) {
        out << "steam_appid = " << info_.steam_appid << "\n";
    }
    if (!info_.game_dir.empty()) {
        out << "game_dir = \"" << info_.game_dir.string() << "\"\n";
    }
    // Per-folder overrides; only non-empty overrides are written.
    if (!info_.mods_dir.empty()) {
        out << "mods_dir = \"" << info_.mods_dir.string() << "\"\n";
    }
    if (!info_.downloads_dir.empty()) {
        out << "downloads_dir = \"" << info_.downloads_dir.string() << "\"\n";
    }
    if (!info_.cache_dir.empty()) {
        out << "cache_dir = \"" << info_.cache_dir.string() << "\"\n";
    }
    if (!info_.profiles_dir.empty()) {
        out << "profiles_dir = \"" << info_.profiles_dir.string() << "\"\n";
    }
    if (!info_.overwrite_dir.empty()) {
        out << "overwrite_dir = \"" << info_.overwrite_dir.string() << "\"\n";
    }
    if (!info_.plugins_txt_path.empty()) {
        out << "plugins_txt_path = \"" << info_.plugins_txt_path.string() << "\"\n";
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

        auto val_start = line.find_first_not_of(" \t", eq + 1);
        if (val_start == std::string::npos) continue;

        // Quoted or numeric value
        if (line[val_start] == '"') {
            val_start++; // skip opening quote
            auto val_end = line.find('"', val_start);
            if (val_end == std::string::npos) continue;
            auto val = line.substr(val_start, val_end - val_start);

            if (key == "game_id") {
                info_.game_id = val;
            } else if (key == "game_dir") {
                info_.game_dir = val;
            } else if (key == "mods_dir") {
                info_.mods_dir = val;
            } else if (key == "downloads_dir") {
                info_.downloads_dir = val;
            } else if (key == "cache_dir") {
                info_.cache_dir = val;
            } else if (key == "profiles_dir") {
                info_.profiles_dir = val;
            } else if (key == "overwrite_dir") {
                info_.overwrite_dir = val;
            } else if (key == "plugins_txt_path") {
                info_.plugins_txt_path = val;
            }
        } else {
            // unquoted numeric/boolean
            auto val_end = line.find_first_of(" \t\r\n", val_start);
            if (val_end == std::string::npos) val_end = line.size();
            auto val = line.substr(val_start, val_end - val_start);

            if (key == "portable") {
                info_.portable = (val == "true");
            } else if (key == "steam_appid") {
                try { info_.steam_appid = std::stoul(val); } catch (...) {}
            }
        }
    }
    return true;
}

std::string Instance::to_instance_name(const std::string& display_name) {
    static const std::string invalid = R"(\/:*?"<>|)";
    std::string result;
    result.reserve(display_name.size());
    for (char c : display_name) {
        if (c == ' ') result += '_';
        else if (invalid.find(c) != std::string::npos) continue;
        else result += c;
    }
    return result;
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
