#include "engine/pipeline/fomod_stage.h"
#include "engine/model/mod.h"
#include "engine/log/logger.h"

#include <filesystem>

namespace engine {

bool FomodStage::execute(Mod& mod, PipelineContext& ctx) {
    (void)ctx;

    // Locate the extracted mod root from the staging dir entry pushed by
    // ExtractStage (same heuristic as InstallStage).
    std::filesystem::path root;
    for (const auto& f : mod.files) {
        auto p = std::filesystem::path(f.relative_path);
        if (std::filesystem::is_directory(p)) {
            root = p;
            break;
        }
    }

    if (root.empty()) {
        Logger::instance().debug("FomodStage: no extracted directory found, nothing to check");
        return true;
    }

    // Standard MO2/Nexus FOMOD marker: fomod/ModuleConfig.xml at the mod root
    // (survives ExtractStage's common-prefix flattening).
    if (std::filesystem::exists(root / "fomod" / "ModuleConfig.xml")) {
        Logger::instance().warn("FomodStage: FOMOD installers are not implemented yet - "
                                "cannot install '" + mod.name + "'");
        return false;
    }

    return true;
}

}  // namespace engine
