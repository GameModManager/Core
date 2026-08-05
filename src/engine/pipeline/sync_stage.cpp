#include "engine/pipeline/sync_stage.h"
#include "engine/pipeline/pipeline.h"
#include "engine/index/conflict_index.h"
#include "engine/instance/instance.h"
#include "engine/log/logger.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <unordered_set>

namespace engine {

bool SyncStage::execute(Mod& mod, PipelineContext& ctx) {
    // Per-mod sync is a no-op - Overwrite capture is a whole-instance
    // operation triggered via capture_overwrite_files() after the game exits.
    return true;
}

std::vector<std::string> SyncStage::capture_overwrite_files(
    const std::filesystem::path& game_dir,
    const std::filesystem::path& overwrite_dir,
    const ConflictIndex& conflict_index) {
    std::vector<std::string> captured;
    std::error_code ec;

    if (!std::filesystem::exists(game_dir)) return captured;
    if (!std::filesystem::exists(overwrite_dir)) {
        std::filesystem::create_directories(overwrite_dir, ec);
    }

    // Build a set of all deployed paths from the ConflictIndex.
    // The index keys are relative paths like "Data/mymod/texture.dds".
    std::unordered_set<std::string> deployed;
    for (const auto& [path, entries] : conflict_index.all()) {
        deployed.insert(path);
    }

    // Walk the game directory
    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             game_dir, std::filesystem::directory_options::skip_permission_denied)) {
        if (!entry.is_regular_file()) continue;

        // Skip symlinks - deployed mod files are symlinked into the game dir
        if (entry.is_symlink()) continue;

        auto rel = std::filesystem::relative(entry.path(), game_dir, ec);
        if (ec) continue;

        auto rel_str = rel.string();

        // Skip files belonging to deployed mods
        if (deployed.count(rel_str)) continue;

        // Skip common runtime artifacts that shouldn't be captured
        auto ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".tmp" || ext == ".temp" || ext == ".bak") continue;

        // Copy to Overwrite
        auto dest = overwrite_dir / rel;
        std::filesystem::create_directories(dest.parent_path(), ec);
        std::filesystem::copy_file(
            entry.path(), dest,
            std::filesystem::copy_options::overwrite_existing, ec);
        if (!ec) {
            captured.push_back(rel_str);
        }
    }

    if (!captured.empty()) {
        Logger::instance().debug(
            "Overwrite capture: " + std::to_string(captured.size()) +
            " file(s) captured from game directory");
    }

    return captured;
}

}  // namespace engine
