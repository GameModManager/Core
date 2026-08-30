#include "engine/deploy/core.h"

#include "engine/core/log/logger.h"
#include "engine/deploy/overlay_fs_deploy.h"
#include "engine/deploy/symlink.h"
#include "engine/game/registry/game_knowledge.h"

#ifdef GMM_PLATFORM_LINUX
#include "engine/deploy/launch/overlay_launcher.h"
#endif

#include <string>

namespace Deploy {

std::unique_ptr<Interface>
Core::create(const std::string &name, bool case_sensitive,
             const std::filesystem::path &staging_dir) {
  // "overlayfs" — Linux-only OverlayFsDeploy when the platform supports it.
#ifdef GMM_PLATFORM_LINUX
  if (name == engine::kDeployStrategyOverlayFs) {
    // overlay_supported() is the gatekeeper; callers should have checked
    // this before requesting a staging dir, but we guard defensively.
    if (!overlay_supported()) {
      engine::Logger::instance().warn(
          "Deploy::Core::create: OverlayFS not supported on this platform, "
          "falling back to Symlink");
      return std::make_unique<Symlink>(case_sensitive);
    }
    if (staging_dir.empty()) {
      engine::Logger::instance().warn(
          "Deploy::Core::create: OverlayFS requested but no staging_dir "
          "provided, falling back to Symlink");
      return std::make_unique<Symlink>(case_sensitive);
    }
    return std::make_unique<OverlayFsDeploy>(staging_dir, case_sensitive);
  }
#else
  (void)staging_dir;
#endif

  // "direct", "symlink", and empty all resolve to Symlink.  The "direct"
  // lifecycle (Deploy::Direct) is constructed by callers that need
  // deploy_all/undeploy/sync; the factory produces the per-file strategy.
  if (name == engine::kDeployStrategyDirect || name.empty() ||
      name == "symlink") {
    return std::make_unique<Symlink>(case_sensitive);
  }

  // Unknown name — log and degrade to Symlink.
  if (!name.empty()) {
    engine::Logger::instance().warn("Deploy::Core::create: unknown strategy '" +
                                    name + "', falling back to Symlink");
  }
  return std::make_unique<Symlink>(case_sensitive);
}

const char *Core::label(const std::string &name) {
#ifdef GMM_PLATFORM_LINUX
  if (name == engine::kDeployStrategyOverlayFs && overlay_supported())
    return "OverlayFS";
#endif
  if (name == engine::kDeployStrategyDirect)
    return "Direct";
  return "Symlink";
}

bool Core::overlay_supported(const std::filesystem::path &overwrite_dir) {
#ifdef GMM_PLATFORM_LINUX
  return engine::OverlayFsLauncher::is_supported(overwrite_dir);
#else
  (void)overwrite_dir;
  return false;
#endif
}

} // namespace Deploy
