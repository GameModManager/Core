#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace engine {
class GameKnowledge;
class Platform;
struct LaunchPrepRequest;
}  // namespace engine

namespace cli {

class HeadlessLauncher {
public:
  struct Config {
    std::filesystem::path executable;
    std::filesystem::path game_dir;
    std::filesystem::path instance_root;
    uint32_t steam_appid                   = 0;
    bool is_windows_exe                    = false;
    const engine::GameKnowledge *knowledge = nullptr;
    std::string game_id;
    // Per-profile local saves (MO2 GamebryoLocalSavegames). Mirrors the GUI
    // Settings::local_saves() toggle (default off). Wired through the shared
    // prepare_launch_params path so both CLI and GUI agree.
    bool local_saves_enabled = false;
    // Explicit per-launch overrides from CLI flags (--args/--env/--cwd).
    // Each *_set flag records whether the flag was passed: an explicit flag
    // always wins over the instance.toml executables entry for that field
    // (documented in --help). Without flags the entry is the default; with
    // neither, the request stays empty (the pre-existing behavior).
    std::vector<std::string> args;
    bool args_set = false;
    std::vector<std::string> environment;
    bool environment_set = false;
    // Raw --cwd value (game-relative or absolute); resolved against game_dir
    // during request assembly so entry and flag cwd share one code path.
    std::filesystem::path cwd;
    bool cwd_set = false;
  };

  explicit HeadlessLauncher(const Config &config, engine::Platform *platform = nullptr);
  int run();

private:
  Config config_;
  engine::Platform *platform_;
};

// Request assembly (no spawn; warns on non-directory cwd): the instance.toml
// executables entry for the configured binary is the default, explicit
// Config flags override it per field. Tested without launching in
// headless_launch_test.cpp.
[[nodiscard]] engine::LaunchPrepRequest
build_launch_request(const HeadlessLauncher::Config &config);

// Parse-time --env validation: keeps entries of the form KEY=VALUE (empty
// value allowed, extra '=' kept verbatim), warns and skips entries without
// '=' or with an empty key - the same entries the launcher would later drop
// with only a launch-time warning. Called where ParsedArgs are mapped so a
// headless typo surfaces before the launch.
[[nodiscard]] std::vector<std::string>
filter_valid_env_entries(const std::vector<std::string> &entries);

}  // namespace cli
