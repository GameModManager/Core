#pragma once

#include "engine/deploy/deploy_utils.h"
#include "engine/game/detect/game_detector.h"
#include "engine/core/instance/instance.h"
#include "engine/game/registry/game_knowledge.h"

#include <filesystem>
#include <string>
#include <vector>

namespace engine {
struct LaunchParams;
class PlatformInterface;
}

namespace engine {

// Canonical instances directory: ~/.local/share/GameModManager/instances/
[[nodiscard]] std::filesystem::path default_instances_dir();

// Override the canonical instances directory (from app Settings at startup).
// Passing an empty path clears the override. The override takes precedence
// over the default XDG location.
void set_instances_dir_override(const std::filesystem::path& dir);

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
//
// display_name (Workspace-4fu): user-chosen instance name. It is sanitized
// with sanitize_directory_name() (spaces preserved) and becomes the folder
// name; creation fails when an instance with that folder already exists.
// An empty or dot-only display_name is refused (returns an Instance with
// empty game_id).
[[nodiscard]] Instance create_instance_for_game(
    const DetectedGame& game,
    const std::filesystem::path& instances_root,
    const std::string& display_name);

// Legacy form: no custom name — folder derived from the game name via
// Instance::to_instance_name (spaces folded to underscores).
[[nodiscard]] Instance create_instance_for_game(
    const DetectedGame& game,
    const std::filesystem::path& instances_root);

// Overload: uses default_instances_dir() as root
[[nodiscard]] inline Instance create_instance_for_game(
    const DetectedGame& game) {
    return create_instance_for_game(game, default_instances_dir());
}

// Snapshot of everything prepare_launch_params needs, captured on the calling
// thread so a worker can run the deploy off the main thread (P8.4). Held by
// value; GameKnowledge is a read-only key/value store copied cheaply.
struct LaunchPrepRequest {
    std::filesystem::path instance_root;
    std::filesystem::path game_dir;
    std::filesystem::path executable;
    GameKnowledge knowledge;
    std::string game_id;
    uint32_t steam_appid = 0;
    bool is_windows_exe = false;
    // Local-saves redirect (MO2 GamebryoLocalSavegames): when true and the
    // game has a registered local_savegames feature, prepare_launch_params
    // rewrites the game INI to save under __MO_Saves and installs the profile
    // dir bind mount into LaunchParams. Only honored for Windows executables
    // when the overlay launcher is available (a bind mount needs the mount
    // namespace). Empty platform = skipped.
    bool local_saves_enabled = false;
    const class PlatformInterface* platform = nullptr;
    // Per-executable environment overrides ("KEY=VALUE"), forwarded verbatim to
    // LaunchParams so the launched process gets them (empty = inherit).
    std::vector<std::string> environment;
    // Per-executable command-line arguments, forwarded verbatim to
    // LaunchParams (empty = none).
    std::vector<std::string> args;
    // Per-executable working directory (empty = game_dir), forwarded verbatim
    // to LaunchParams.
    std::filesystem::path cwd;
};

// Direct-symlink deploy parameters gathered once from instance.toml + game
// knowledge. The single source of truth for direct-mode deploys: both
// prepare_launch_params and the UI's "Deploy management" actions consume it
// (golden rule: never re-derive these per caller).
struct DeployConfig {
    std::filesystem::path mods_dir;
    std::filesystem::path game_dir;
    // Deploy-target override (instance.toml "game_mods_dir"): the game's
    // actual mods folder when it lives outside the install dir (Isaac on
    // macOS). Empty = deploy into game_dir via deploy_prefix, exactly like
    // before this field existed. When set it IS the mods folder: mod files
    // land directly in it and deploy_prefix is not appended (see
    // deploy_config_for).
    std::filesystem::path game_mods_dir;
    std::string deploy_prefix;
    bool deploy_include_mod_id = false;
    std::string disable_mechanism;
    bool case_sensitive = true;
    std::filesystem::path ledger_file;
    // Where originals displaced by the deploy are parked (<game_dir>/
    // kOriginalFilesDirName); empty when a caller opts out of backup/restore.
    std::filesystem::path backup_root;

    // Effective deploy root for direct/symlink strategies: the override when
    // set, else game_dir. The single resolution point every deploy consumer
    // (prepare_launch_params, Deploy Management) goes through.
    [[nodiscard]] std::filesystem::path deploy_target() const {
        return game_mods_dir.empty() ? game_dir : game_mods_dir;
    }
};

// Gather the DeployConfig for a direct-symlink deploy of an instance's enabled
// mods into its game dir. Honors the same knowledge keys and GMM_CASE_SENSITIVE
// override as prepare_launch_params, so a UI-initiated re-deploy or removal
// behaves exactly like the launch-time deploy.
[[nodiscard]] DeployConfig deploy_config_for(
    const std::filesystem::path& instance_root,
    const std::filesystem::path& game_dir,
    const GameKnowledge& knowledge,
    const std::string& game_id);

// Effective deploy strategy for an instance: the per-instance
// "deploy_strategy" override in instance.toml when set, else the game plugin's
// knowledge default (deploy_strategy_for). The single source of truth both the
// launch path and the UI's strategy selector consume, so a user picking a
// strategy in Deploy Management is honored at launch.
[[nodiscard]] std::string effective_deploy_strategy(
    const std::filesystem::path& instance_root,
    const GameKnowledge& knowledge,
    const std::string& game_id);

// Prepare LaunchParams for launching a game from an instance.
// If OverlayFS is supported on the instance:
//   - Ensures .gmm_staging exists
//   - Deploys all enabled mods to staging (creates symlinks)
//   - Populates extra_lowerdirs with staging dir
// Otherwise: returns basic LaunchParams with no extra lowerdirs.
//
// The deploy runs on the parallel executor (PLAN §13.3). progress, if set, is
// invoked with link-operation progress as the deploy runs (see
// deploy_utils.h); the engine function itself is synchronous and only returns
// once the staging tree is fully populated.
[[nodiscard]] LaunchParams prepare_launch_params(
    const LaunchPrepRequest& req,
    const DeployProgressFn& progress = {});

// Convenience overload keeping the old argument order (tests, CLI).
[[nodiscard]] LaunchParams prepare_launch_params(
    const std::filesystem::path& instance_root,
    const std::filesystem::path& game_dir,
    const std::filesystem::path& executable,
    const GameKnowledge& knowledge,
    const std::string& game_id,
    uint32_t steam_appid,
    bool is_windows_exe);

}
