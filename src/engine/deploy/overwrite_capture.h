#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace engine {

// Captures runtime-generated files from the game directory into the Overwrite mod.
// After the game runs, any files in the game directory that don't belong to a
// deployed mod get swept into the instance's Overwrite folder - configs, shader
// caches, logs, save-related files, etc.
//
// This is the mechanism that makes "Overwrite captures everything" real.
class OverwriteCapture {
public:
    struct Config {
        std::filesystem::path game_dir;       // live game directory
        std::filesystem::path overwrite_dir;  // instance's mods/Overwrite/
        std::vector<std::string> deployed_relative_paths;  // all files currently deployed
        std::vector<std::string> ignored_patterns;         // file patterns to skip
    };

    // Scan game_dir, copy any file not in deployed_relative_paths into overwrite_dir.
    // Returns the list of captured relative paths.
    [[nodiscard]] static std::vector<std::string> capture(const Config& config);

    // Clear the Overwrite directory (e.g. before a fresh deploy).
    [[nodiscard]] static bool clear(const std::filesystem::path& overwrite_dir);

    // Promote files from Overwrite into a proper mod folder.
    [[nodiscard]] static bool promote_to_mod(
        const std::filesystem::path& overwrite_dir,
        const std::filesystem::path& mod_dir,
        const std::vector<std::string>& relative_paths);

private:
    static bool should_ignore(const std::string& relative_path,
                              const std::vector<std::string>& patterns);
};

}  // namespace engine
