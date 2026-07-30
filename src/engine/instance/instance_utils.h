#pragma once

#include "engine/detect/game_detector.h"
#include "engine/instance/instance.h"

#include <filesystem>
#include <string>
#include <vector>

namespace engine {
struct LaunchParams;
class GameKnowledge;
}

namespace engine {

// Canonical instances directory: ~/.local/share/GameModManager/instances/
[[nodiscard]] std::filesystem::path default_instances_dir();

// Canonical last_instance file path
[[nodiscard]] std::filesystem::path last_instance_file_path();

// Read/write the last-used instance name
[[nodiscard]] std::string read_last_instance();
void write_last_instance(const std::string& name);

// Scan for existing instances. Returns instance names (directory basenames).
[[nodiscard]] std::vector<std::string> scan_instances();

// Resolve an instance path from a short name or absolute path.
// If name_or_path is absolute, checks if instance.toml exists at that path.
// If relative, checks under default_instances_dir()/name_or_path.
// Returns empty path if not found.
[[nodiscard]] std::filesystem::path resolve_instance_path(
    const std::string& name_or_path);

// Create an instance for a detected game under the given instances root.
// Returns the fully initialized Instance (dirs created, toml written).
// Returns an Instance with empty game_id on failure.
[[nodiscard]] Instance create_instance_for_game(
    const DetectedGame& game,
    const std::filesystem::path& instances_root);

// Overload: uses default_instances_dir() as root
[[nodiscard]] inline Instance create_instance_for_game(
    const DetectedGame& game) {
    return create_instance_for_game(game, default_instances_dir());
}

// Prepare LaunchParams for launching a game from an instance.
// If OverlayFS is supported on the instance:
//   - Ensures .gmm_staging exists
//   - Deploys all enabled mods to staging (creates symlinks)
//   - Populates extra_lowerdirs with staging dir
// Otherwise: returns basic LaunchParams with no extra lowerdirs.
[[nodiscard]] LaunchParams prepare_launch_params(
    const std::filesystem::path& instance_root,
    const std::filesystem::path& game_dir,
    const std::filesystem::path& executable,
    const GameKnowledge& knowledge,
    const std::string& game_id,
    uint32_t steam_appid,
    bool is_windows_exe);

}
