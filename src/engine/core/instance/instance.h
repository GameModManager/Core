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
    Meta,
};

struct InstanceInfo {
    std::string game_id;
    // User-friendly instance name as the user typed it (may contain spaces,
    // colons, ...). Persisted to instance.toml as "name"; the folder uses the
    // sanitized form (see Instance::to_instance_name). Empty for instances
    // created before the field existed — display falls back to the folder
    // basename (instance_display_name()).
    std::string display_name;
    std::filesystem::path root;
    std::filesystem::path game_dir;  // path to the actual game install (e.g. steamapps/common/...)
    // The game's actual mods folder — the deploy target — when it lives
    // outside the install dir. Resolution order (Workspace-otx): this
    // per-instance override first, then the plugin-declared "game_mods_dir"
    // knowledge hook (Isaac on macOS:
    // ~/Library/Application Support/Binding of Isaac Afterbirth+ Mods),
    // then game_dir via the plugin's deploy_prefix. When set it IS the mods
    // folder: mod files land directly in it (no deploy_prefix appended).
    // Persisted to instance.toml only when non-empty.
    std::filesystem::path game_mods_dir;
    uint32_t steam_appid = 0;
    bool portable = true;
    // Per-folder overrides for the instance's working directories. Empty
    // means "use the default": <root>/mods, <root>/downloads, ... (MO2's
    // base_directory-relative defaults). Persisted to instance.toml only
    // when non-empty.
    std::filesystem::path mods_dir;
    std::filesystem::path downloads_dir;
    std::filesystem::path cache_dir;
    std::filesystem::path profiles_dir;
    std::filesystem::path overwrite_dir;
    // Absolute path for the game's Plugins.txt (MO2's
    // "Ignore plugins.txt on first launch"-adjacent override). Empty means
    // "resolve via platform" (e.g. Proton prefix AppData/Local). Persisted to
    // instance.toml only when non-empty.
    std::filesystem::path plugins_txt_path;
    // Selected Proton runner for this instance (display name or absolute path
    // to a `proton` script). Empty = automatic (Steam per-game override, then
    // latest installed Proton). Persisted to instance.toml.
    std::string proton_runner;
    // Per-instance deploy strategy override ("symlink" | "overlayfs"). Empty
    // = the game plugin's "deploy_strategy" knowledge default. Persisted to
    // instance.toml; wins over the knowledge key when set.
    std::string deploy_strategy;
    // Name of the last selected right-panel tab (capability key, e.g.
    // "plugins", "downloads", "data"). Empty = default to the first tab.
    // Persisted to instance.toml.
    std::string last_tab;
};

class Instance {
public:
    static Instance portable(const std::filesystem::path& root);
    static Instance installed(const std::string& name,
                             const std::filesystem::path& instances_root);
    static Instance from_root(const std::filesystem::path& root);

    [[nodiscard]] InstanceInfo& info() { return info_; }
    [[nodiscard]] const InstanceInfo& info() const { return info_; }
    [[nodiscard]] std::filesystem::path path_for(InstanceKind kind) const;
    [[nodiscard]] std::filesystem::path toml_path() const;

    // Set/clear a per-folder override (empty clears -> default under root).
    void set_path_override(InstanceKind kind, const std::filesystem::path& path);
    [[nodiscard]] std::filesystem::path path_override(InstanceKind kind) const;

    // Surgically set/replace a single string key in instance.toml without
    // touching any other keys (the file also holds app-owned sections like
    // `executables`, so a full rewrite would clobber them). Removes the key
    // when `value` is empty. Returns false on I/O failure.
    bool write_key(const std::string& key, const std::string& value) const;

    bool create_directories() const;
    bool write_toml() const;
    bool read_toml();

    // Convert a display name to a filesystem-safe instance folder name.
    // Spaces are kept (safe on all platforms); stripped: /\:*?"<>| and
    // control characters (incl. NUL); leading/trailing dots and whitespace
    // are trimmed so ".", ".." and "..." degrade to "".
    // "The Binding of Isaac: Rebirth" → "The Binding of Isaac Rebirth"
    // "My Skyrim Setup"               → "My Skyrim Setup"
    // Returns "" for degenerate input; callers pick a fallback
    // (unique_instance_name does).
    static std::string to_instance_name(const std::string& display_name);

    static std::filesystem::path resolve_portable_root(
        const std::filesystem::path& exe_dir);
    static bool is_portable(const std::filesystem::path& exe_dir);

    // Default-constructed instance (empty root). Use portable()/installed()/
    // from_root() for real instances; the default ctor exists so clients can
    // hold a by-value member that is only populated once a root is known.
    Instance() = default;

private:
    InstanceInfo info_;
};

}  // namespace engine
