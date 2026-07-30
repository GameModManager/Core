#pragma once

#include "platform/platform_interface.h"

#include <cstdlib>
#include <filesystem>
#include <string>

namespace engine {

// Linux platform module — XDG paths, Proton/Wine discovery, FUSE capabilities.
class LinuxPlatform : public PlatformInterface {
public:
    [[nodiscard]] std::string platform_name() const override { return "linux"; }

    [[nodiscard]] std::filesystem::path data_dir() const override;
    [[nodiscard]] std::filesystem::path config_dir() const override;
    [[nodiscard]] std::filesystem::path cache_dir() const override;

    [[nodiscard]] std::filesystem::path find_steam_root() const override;
    [[nodiscard]] std::filesystem::path find_proton() const override;
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
};

}  // namespace engine
