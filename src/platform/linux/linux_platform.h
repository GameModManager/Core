#pragma once

#include "platform/platform_interface.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace engine {

// Linux platform module - XDG paths, Proton/Wine discovery, FUSE capabilities.
class LinuxPlatform : public PlatformInterface {
public:
    [[nodiscard]] std::string platform_name() const override { return "linux"; }

    [[nodiscard]] std::filesystem::path data_dir() const override;
    [[nodiscard]] std::filesystem::path config_dir() const override;
    [[nodiscard]] std::filesystem::path cache_dir() const override;

    [[nodiscard]] std::filesystem::path find_steam_root() const override;
    [[nodiscard]] std::filesystem::path find_proton() const override;
    [[nodiscard]] std::filesystem::path find_proton_for_game(uint32_t steam_appid) const override;
    [[nodiscard]] std::vector<ProtonVersionInfo> enumerate_proton_versions() const override;
    [[nodiscard]] std::filesystem::path find_proton_named(const std::string& name) const override;
    [[nodiscard]] std::filesystem::path resolve_proton_prefix(uint32_t steam_appid) const override;
    [[nodiscard]] std::vector<std::filesystem::path> steam_library_paths() const override;
    [[nodiscard]] std::filesystem::path game_documents_dir(uint32_t steam_appid) const override;
    [[nodiscard]] std::filesystem::path game_local_appdata_dir(uint32_t steam_appid) const override;
    [[nodiscard]] std::filesystem::path find_wine() const override;

    [[nodiscard]] bool launch_executable(
        const std::filesystem::path& executable,
        const std::vector<std::string>& args = {}) const override;

    [[nodiscard]] bool is_elevated() const override;
    [[nodiscard]] bool symlinks_available() const override { return true; }
    [[nodiscard]] bool junctions_available() const override { return false; }

    // NXM protocol handler registration (XDG compliant)
    [[nodiscard]] static bool register_nxm_handler(const std::filesystem::path& exe_path);
    [[nodiscard]] static bool unregister_nxm_handler();
    [[nodiscard]] static bool is_nxm_handler_registered();

    // GMM custom protocol handler registration (for gmm:// links)
    [[nodiscard]] static bool register_gmm_handler(const std::filesystem::path& exe_path);
    [[nodiscard]] static bool unregister_gmm_handler();
    [[nodiscard]] static bool is_gmm_handler_registered();

private:
    static std::filesystem::path resolve_env_dir(
        const char* env_var, const std::filesystem::path& fallback);

    // VDF parsing helpers (Steam config files)
    static std::string vdf_value_for_key(const std::string& line, const std::string& key);
    // Per-game Proton tool override from Steam's config.vdf ("proton_experimental", ...)
    std::string read_steam_compat_tool(uint32_t steam_appid) const;
    // Map a tool name to its install directory via compatibilitytool.vdf
    std::filesystem::path resolve_tool_dir(const std::string& tool_name) const;
    // Every Proton runner Steam manages: dirs under steamapps/common with a
    // `proton` script, plus compatibility tool entries (GE-Proton etc.).
    // `binary` is only non-empty when a `proton` script actually exists.
    std::vector<ProtonVersionInfo> scan_proton_runners() const;
    // Windows user profile dir inside a Proton prefix ("drive_c/users/<user>"),
    // resolved by scanning drive_c/users/ (preferring "steamuser"). Empty when
    // the prefix is empty or has no user dir.
    static std::filesystem::path prefix_user_dir(const std::filesystem::path& prefix);
};

}  // namespace engine
