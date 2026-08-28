#include "engine/pipeline/deploy_stage.h"
#include "engine/pipeline/pipeline.h"
#include "engine/deploy/strategy.h"
#include "engine/core/log/logger.h"
#include "engine/core/vfs/path_resolver.h"

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
    {
        // Resolve the deploy target base through PathResolver so a re-cased
        // on-disk layout (e.g. "data" instead of "Data") deploys into the real
        // directory rather than creating a second, wrongly-cased one. The staging
        // dir (when used) is GMM-managed but the same casing risk applies, so we
        // resolve against whichever root actually receives the deploy.
        const std::filesystem::path& root =
            ctx.staging_dir.empty() ? ctx.game_dir : ctx.staging_dir;
        engine::vfs::PathResolver resolver(root);
        auto resolved = resolver.resolve(ctx.deploy_prefix);
        std::error_code ec2;
        if (resolved && resolved->exists() &&
            std::filesystem::is_directory(resolved->absolute(), ec2)) {
            target_base = resolved->absolute();
        } else {
            target_base = root / ctx.deploy_prefix;  // fallback: create as requested
        }
    }
    std::error_code ec;
    std::filesystem::create_directories(target_base, ec);

    int deployed = 0;
    int failed = 0;
    auto deploy_root = ctx.deploy_include_mod_id ? target_base / mod.id : target_base;
    // skip_permission_denied: a permission-denied subdirectory makes the
    // range-for's throwing operator++ abort the whole deploy (SIGABRT).
    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             mod_path, std::filesystem::directory_options::skip_permission_denied)) {
        if (entry.is_regular_file()) {
            auto rel = std::filesystem::relative(entry.path(), mod_path);
            // NOTE: no create_directories(target.parent_path()) here. The
            // strategy owns dir creation; pre-creating the exact-cased parent
            // would defeat the case-insensitive target merge.
            auto target = deploy_root / rel;
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
