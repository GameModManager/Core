#include "engine/core/instance/instance.h"

#include "engine/core/instance/toml_utils.h"

#include <fstream>

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
    toml::table tbl;
    tbl.emplace("game_id", info_.game_id);
    tbl.emplace("portable", info_.portable);
    if (info_.steam_appid > 0) {
        tbl.emplace("steam_appid", static_cast<int64_t>(info_.steam_appid));
    }
    if (!info_.game_dir.empty()) {
        tbl.emplace("game_dir", info_.game_dir.string());
    }
    if (!info_.game_mods_dir.empty()) {
        tbl.emplace("game_mods_dir", info_.game_mods_dir.string());
    }
    // Per-folder overrides; only non-empty overrides are written.
    if (!info_.mods_dir.empty()) {
        tbl.emplace("mods_dir", info_.mods_dir.string());
    }
    if (!info_.downloads_dir.empty()) {
        tbl.emplace("downloads_dir", info_.downloads_dir.string());
    }
    if (!info_.cache_dir.empty()) {
        tbl.emplace("cache_dir", info_.cache_dir.string());
    }
    if (!info_.profiles_dir.empty()) {
        tbl.emplace("profiles_dir", info_.profiles_dir.string());
    }
    if (!info_.overwrite_dir.empty()) {
        tbl.emplace("overwrite_dir", info_.overwrite_dir.string());
    }
    if (!info_.plugins_txt_path.empty()) {
        tbl.emplace("plugins_txt_path", info_.plugins_txt_path.string());
    }
    if (!info_.proton_runner.empty()) {
        tbl.emplace("proton_runner", info_.proton_runner);
    }
    if (!info_.deploy_strategy.empty()) {
        tbl.emplace("deploy_strategy", info_.deploy_strategy);
    }
    if (!info_.last_tab.empty()) {
        tbl.emplace("last_tab", info_.last_tab);
    }

    std::ofstream out(toml_path());
    if (!out) return false;
    out << serialize_instance_toml(tbl);
    return out.good();
}

bool Instance::read_toml() {
    auto tbl = parse_instance_toml(toml_path());
    if (!tbl) return false;

    if (auto v = (*tbl)["game_id"].value<std::string>()) {
        info_.game_id = *v;
    }
    if (auto v = (*tbl)["game_dir"].value<std::string>()) {
        info_.game_dir = *v;
    }
    if (auto v = (*tbl)["game_mods_dir"].value<std::string>()) {
        info_.game_mods_dir = *v;
    }
    if (auto v = (*tbl)["mods_dir"].value<std::string>()) {
        info_.mods_dir = *v;
    }
    if (auto v = (*tbl)["downloads_dir"].value<std::string>()) {
        info_.downloads_dir = *v;
    }
    if (auto v = (*tbl)["cache_dir"].value<std::string>()) {
        info_.cache_dir = *v;
    }
    if (auto v = (*tbl)["profiles_dir"].value<std::string>()) {
        info_.profiles_dir = *v;
    }
    if (auto v = (*tbl)["overwrite_dir"].value<std::string>()) {
        info_.overwrite_dir = *v;
    }
    if (auto v = (*tbl)["plugins_txt_path"].value<std::string>()) {
        info_.plugins_txt_path = *v;
    }
    if (auto v = (*tbl)["proton_runner"].value<std::string>()) {
        info_.proton_runner = *v;
    }
    if (auto v = (*tbl)["deploy_strategy"].value<std::string>()) {
        info_.deploy_strategy = *v;
    }
    if (auto v = (*tbl)["last_tab"].value<std::string>()) {
        info_.last_tab = *v;
    }
    if (auto v = (*tbl)["portable"].value<bool>()) {
        info_.portable = *v;
    }
    if (auto v = (*tbl)["steam_appid"].value<int64_t>()) {
        info_.steam_appid = static_cast<uint32_t>(*v);
    }
    return true;
}

bool Instance::write_key(const std::string& key, const std::string& value) const {
    auto path = toml_path();
    // Read-modify-write: parse the full file (legacy repair included) so
    // app-owned sections like `executables` survive untouched. A missing or
    // unparseable file starts from an empty table.
    auto tbl = parse_instance_toml(path);
    if (!tbl) {
        tbl = toml::table{};
    }
    if (value.empty()) {
        tbl->erase(key);
    } else {
        tbl->insert_or_assign(key, value);
    }

    std::ofstream out(path);
    if (!out) return false;
    out << serialize_instance_toml(*tbl);
    return out.good();
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
