#pragma once

#include "platform/platform_interface.h"

#include <filesystem>
#include <string>

namespace engine {

// Windows platform module — registry paths, native launch, Steam detection via
// registry or common paths. Does NOT compile on Linux (per PLAN.md §11 — platform
// code is only in the translation units for its own OS).
class WindowsPlatform : public PlatformInterface {
public:
    [[nodiscard]] std::string platform_name() const override { return "windows"; }

    [[nodiscard]] std::filesystem::path data_dir() const override;
    [[nodiscard]] std::filesystem::path config_dir() const override;
    [[nodiscard]] std::filesystem::path cache_dir() const override;

    [[nodiscard]] std::filesystem::path find_steam_root() const override;
    [[nodiscard]] std::filesystem::path find_proton() const override { return {}; }

    [[nodiscard]] bool launch_executable(
        const std::filesystem::path& executable,
        const std::vector<std::string>& args = {}) const override;

    [[nodiscard]] bool is_elevated() const override;
    [[nodiscard]] bool symlinks_available() const override;
    [[nodiscard]] bool junctions_available() const override { return true; }

    // Windows-specific: read a string value from the Windows registry.
    // Returns empty path if the key/value doesn't exist or on error.
    [[nodiscard]] static std::filesystem::path registry_read_string(
        const std::wstring& key_path, const std::wstring& value_name);

    // Windows-specific: register an nxm:// protocol handler.
    [[nodiscard]] static bool register_nxm_handler(
        const std::filesystem::path& exe_path);

    // Windows-specific: unregister the nxm:// protocol handler.
    [[nodiscard]] static bool unregister_nxm_handler();

private:
    static std::filesystem::path appdata_dir() const;
    static std::filesystem::path localappdata_dir() const;
};

}  // namespace engine
