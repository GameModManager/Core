#include "engine/pipeline/deploy_stage.h"
#include "engine/pipeline/pipeline.h"
#include "engine/deploy/strategy.h"
#include "engine/log/logger.h"

namespace engine {

bool DeployStage::execute(Mod& mod, PipelineContext& ctx) {
    if (!ctx.deploy_strategy) {
        Logger::instance().warn("DeployStage: no deploy strategy configured, skipping");
        mod.state = ModState::Deployed;
        return true;
    }

    if (ctx.game_dir.empty()) {
        Logger::instance().error("DeployStage: no game_dir in context");
        return false;
    }

    auto mod_path = ctx.mods_dir / mod.id;
    if (!std::filesystem::exists(mod_path)) {
        Logger::instance().error("DeployStage: mod path not found: " + mod_path.string());
        return false;
    }

    std::filesystem::path target_base;
    if (!ctx.staging_dir.empty()) {
        target_base = ctx.staging_dir / ctx.deploy_prefix;
    } else {
        target_base = ctx.game_dir / ctx.deploy_prefix;
    }
    std::error_code ec;
    std::filesystem::create_directories(target_base, ec);

    int deployed = 0;
    int failed = 0;
    auto deploy_root = ctx.deploy_include_mod_id ? target_base / mod.id : target_base;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(mod_path)) {
        if (entry.is_regular_file()) {
            auto rel = std::filesystem::relative(entry.path(), mod_path);
            auto target = deploy_root / rel;
            std::filesystem::create_directories(target.parent_path(), ec);
            if (ctx.deploy_strategy->deploy(entry.path(), target)) {
                deployed++;
            } else {
                failed++;
            }
        }
    }

    Logger::instance().debug("DeployStage: deployed " + std::to_string(deployed) +
                            " files" + (failed ? ", " + std::to_string(failed) + " failed" : "") +
                            " for " + mod.id);
    mod.state = ModState::Deployed;
    return failed == 0;
}

} // namespace engine
