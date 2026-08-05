#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace engine {

class GameKnowledge;

struct ScannedMod {
    std::string folder_name;     // directory name on disk
    std::string display_name;    // from metadata, normalized (no priority prefix)
    std::string raw_name;        // from metadata, as-is (with priority prefix)
    std::string version;         // from metadata
    std::string separator_color; // hex color for separators (e.g. "#888888"), empty for mods
    int priority = -1;           // extracted from name prefix via priority_prefix_re, -1 = none
    int64_t workshop_id = 0;     // extracted from folder name via workshop_id_pattern, 0 = none
    bool enabled = true;         // false if disable sentinel exists
    bool is_separator = false;   // true if folder ends with separator_suffix
    bool is_overwrite = false;   // true for the special Overwrite entry
    bool is_game_native = false; // true for game-provided plugins (e.g. vanilla ESMs)
    bool is_fomod = false;       // true if meta.ini carries a [fomod] section with saved choices
    bool root_override = false;  // true if the mod deploys to the game root (meta.ini [General] rootOverride)
};

// Generic mod scanner - reads ALL game-specific config from GameKnowledge.
// No hardcoded file formats, tag names, or folder conventions.
// Each game plugin tells the engine how to discover and parse its mods.
class ModScanner {
public:
    // Scan a game's mods directory using settings from GameKnowledge.
    // game_id is used to look up hooks in knowledge.
    [[nodiscard]] static std::vector<ScannedMod> scan(
        const GameKnowledge& knowledge,
        const std::string& game_id,
        const std::filesystem::path& game_install_dir,
        const std::vector<std::filesystem::path>& ignore_symlink_targets = {});

    // Scan a specific mods directory directly (bypasses mods_subpath resolution).
    [[nodiscard]] static std::vector<ScannedMod> scan_dir(
        const GameKnowledge& knowledge,
        const std::string& game_id,
        const std::filesystem::path& mods_dir,
        const std::vector<std::filesystem::path>& ignore_symlink_targets = {});

    // Scan a single mod folder (installed_missing_stages: the install pipeline
    // produces one folder at a time, so the UI can add just that row instead of
    // rescanning the whole mods dir). Returns empty when the folder holds no
    // recognized mod.
    [[nodiscard]] static std::vector<ScannedMod> scan_folder(
        const GameKnowledge& knowledge,
        const std::string& game_id,
        const std::filesystem::path& mods_dir,
        const std::string& folder_name,
        const std::vector<std::filesystem::path>& ignore_symlink_targets = {});

    // Create the disable sentinel file for a mod.
    [[nodiscard]] static bool disable_mod(
        const GameKnowledge& knowledge,
        const std::string& game_id,
        const std::filesystem::path& mod_folder);

    // Remove the disable sentinel file to enable a mod.
    [[nodiscard]] static bool enable_mod(
        const GameKnowledge& knowledge,
        const std::string& game_id,
        const std::filesystem::path& mod_folder);

    // Set the priority of a mod by rewriting its metadata.
    [[nodiscard]] static bool set_priority(
        const GameKnowledge& knowledge,
        const std::string& game_id,
        const std::filesystem::path& mod_folder,
        int priority);
};

}  // namespace engine
