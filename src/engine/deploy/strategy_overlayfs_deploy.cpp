#include "engine/core/log/logger.h"
#include "engine/deploy/deploy_utils.h"
#include "engine/deploy/overlay_fs_deploy.h"

#include <filesystem>
#include <system_error>

namespace Deploy {

OverlayFsDeploy::OverlayFsDeploy(std::filesystem::path staging_dir,
                                 bool case_sensitive)
    : staging_dir_(std::move(staging_dir)), case_sensitive_(case_sensitive) {}

bool OverlayFsDeploy::deploy(const std::filesystem::path &source,
                             const std::filesystem::path &target) {
  std::error_code ec;
  const std::filesystem::path merged =
      case_sensitive_ ? target : engine::resolve_deploy_target_ci(target);
  std::filesystem::create_directories(merged.parent_path(), ec);
  if (ec) {
    engine::Logger::instance().error("OverlayFS deploy: failed to create dir " +
                                     merged.parent_path().string() + ": " +
                                     ec.message());
    return false;
  }

  // Redeploys write into the same staging dir on every launch, so the target
  // already exists on the second run.  Clear it first: create_symlink would
  // otherwise fail with EEXIST and a stale entry (from a removed/renamed mod
  // file) would linger in the overlay.
  std::filesystem::remove(merged, ec);
  if (ec) {
    engine::Logger::instance().error(
        "OverlayFS deploy: failed to clear stale target " + merged.string() +
        ": " + ec.message());
    return false;
  }

  // Executables and scripts must be REAL files in the merged view, not
  // symlinks: they resolve siblings relative to their own location, and a
  // symlinked lowerdir inode resolves through to the mod folder (so
  // skse64_loader.exe would look for SkyrimSE.exe in the mod folder).
  if (engine::is_executable_binary(source)) {
    std::filesystem::copy_file(
        source, merged, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
      engine::Logger::instance().error(
          "OverlayFS deploy: failed to copy binary " + source.string() +
          " -> " + merged.string() + ": " + ec.message());
      return false;
    }
    // Ensure the staged copy carries the exec bit (copy_file preserves the
    // source mode; a mod-shipped exe may lack +x). Pre-setting it here
    // keeps the launcher's chmod a no-op, avoiding an overlay copy-up of
    // the binary into the overwrite upperdir on launch.
    std::error_code perm_ec;
    auto perms = std::filesystem::status(merged, perm_ec).permissions();
    if (!perm_ec && (perms & std::filesystem::perms::owner_exec) ==
                        std::filesystem::perms::none) {
      std::filesystem::permissions(merged,
                                   perms | std::filesystem::perms::owner_exec |
                                       std::filesystem::perms::group_exec |
                                       std::filesystem::perms::others_exec,
                                   perm_ec);
      if (perm_ec) {
        engine::Logger::instance().error(
            "OverlayFS deploy: failed to set exec bit on " + merged.string() +
            ": " + perm_ec.message());
        return false;
      }
    }
    return true;
  }

  std::filesystem::create_symlink(source, merged, ec);
  if (ec) {
    engine::Logger::instance().error("OverlayFS deploy: failed to symlink " +
                                     merged.string() + " -> " +
                                     source.string() + ": " + ec.message());
    return false;
  }
  return true;
}

bool OverlayFsDeploy::remove(const std::filesystem::path &target) {
  std::error_code ec;
  std::filesystem::remove(target, ec);
  return true;
}

void OverlayFsDeploy::set_mod_paths(
    const std::vector<std::filesystem::path> &paths) {
  mod_paths_ = paths;
}

const std::vector<std::filesystem::path> &OverlayFsDeploy::mod_paths() const {
  return mod_paths_;
}

} // namespace Deploy
