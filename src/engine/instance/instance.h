#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace engine {

enum class InstanceKind {
    Mods,
    Profiles,
    Downloads,
    Cache,
    CacheArchives,
    CacheThumbnails,
    Plugins,
    Logs,
    Config,
    Overwrite,
};

struct InstanceInfo {
    std::string game_id;
    std::filesystem::path root;
    std::filesystem::path game_dir;  // path to the actual game install (e.g. steamapps/common/...)
    bool portable = true;
};

class Instance {
public:
    static Instance portable(const std::filesystem::path& root);
    static Instance installed(const std::string& name,
                             const std::filesystem::path& instances_root);

    [[nodiscard]] InstanceInfo& info() { return info_; }
    [[nodiscard]] const InstanceInfo& info() const { return info_; }
    [[nodiscard]] std::filesystem::path path_for(InstanceKind kind) const;
    [[nodiscard]] std::filesystem::path toml_path() const;

    bool create_directories() const;
    bool write_toml() const;
    bool read_toml();

    static std::filesystem::path resolve_portable_root(
        const std::filesystem::path& exe_dir);
    static bool is_portable(const std::filesystem::path& exe_dir);

private:
    Instance() = default;
    InstanceInfo info_;
};

}  // namespace engine
