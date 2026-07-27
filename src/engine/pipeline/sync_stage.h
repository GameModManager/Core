#pragma once

#include "engine/pipeline/stage.h"

#include <filesystem>
#include <string>
#include <vector>

namespace engine {

class ConflictIndex;

class SyncStage : public Stage {
public:
    bool execute(Mod& mod, PipelineContext& ctx) override;

    // Capture runtime-generated files from the game directory into Overwrite.
    // Scans game_dir, finds files not tracked by the ConflictIndex (i.e. not
    // deployed by any mod), and copies them to the Overwrite directory.
    // Returns the list of captured relative paths.
    [[nodiscard]] static std::vector<std::string> capture_overwrite_files(
        const std::filesystem::path& game_dir,
        const std::filesystem::path& overwrite_dir,
        const ConflictIndex& conflict_index);

    // Clear the Overwrite directory (e.g. before a fresh deploy).
    [[nodiscard]] static bool clear_overwrite(
        const std::filesystem::path& overwrite_dir);

    // Promote files from Overwrite into a proper mod directory.
    [[nodiscard]] static bool promote_to_mod(
        const std::filesystem::path& overwrite_dir,
        const std::filesystem::path& mod_dir,
        const std::vector<std::string>& relative_paths);
};

}  // namespace engine
