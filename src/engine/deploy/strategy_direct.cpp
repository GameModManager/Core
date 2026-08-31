#include "engine/core/log/logger.h"
#include "engine/deploy/deploy_utils.h"
#include "engine/deploy/direct.h"

#include <filesystem>
#include <utility>

namespace Deploy {

Direct::Direct(Config config)
    : config_(std::move(config)), file_strategy_(config_.case_sensitive) {}

// Per-file deploy: delegates to Symlink (Liskov-compatible).
bool Direct::deploy(const std::filesystem::path &source,
                    const std::filesystem::path &target) {
  return file_strategy_.deploy(source, target);
}

// Per-file remove: delegates to Symlink.
bool Direct::remove(const std::filesystem::path &target) {
  return file_strategy_.remove(target);
}

// Full deploy: wraps deploy_all_enabled_mods_direct with owned config.
bool Direct::deploy_all(const engine::DeployProgressFn &progress) {
  return engine::deploy_all_enabled_mods_direct(
      config_.mods_dir, config_.game_dir, config_.deploy_prefix,
      config_.deploy_include_mod_id, config_.disable_mechanism,
      config_.case_sensitive, config_.ledger_file, config_.backup_root,
      /*num_threads=*/0, progress);
}

// Undeploy: wraps remove_deployed_files with owned config.
bool Direct::undeploy(const engine::DeployProgressFn &progress) {
  return engine::remove_deployed_files(config_.game_dir, config_.backup_root,
                                       config_.ledger_file,
                                       /*num_threads=*/0, progress);
}

// Incremental sync: deploy_impl already performs the O(Δ) diff internally via
// the ledger — unchanged entries are skipped (one stat), new/re-pointed
// entries are linked, stale entries are unlinked. We wrap deploy_all and
// derive the SyncResult counts by diffing the ledger before/after.
SyncResult Direct::sync(const engine::DeployProgressFn &progress) {
  SyncResult result;
  const auto old_ledger = engine::load_deploy_ledger(config_.ledger_file);
  const bool ok = deploy_all(progress);
  const auto new_ledger = engine::load_deploy_ledger(config_.ledger_file);

  for (const auto &[target, source] : new_ledger) {
    const auto it = old_ledger.find(target);
    if (it == old_ledger.end() || it->second != source)
      ++result.files_deployed;
    else
      ++result.files_unchanged;
  }
  for (const auto &[target, src] : old_ledger) {
    (void)src;
    if (new_ledger.find(target) == new_ledger.end())
      ++result.files_removed;
  }
  if (!ok)
    result.files_failed = -1; // sentinel: partial failure
  return result;
}

bool Direct::is_deployed(const std::filesystem::path &target) const {
  auto ledger = engine::load_deploy_ledger(config_.ledger_file);
  return ledger.count(target) > 0;
}

const DeployedFileInfo *
Direct::find(const std::filesystem::path &target) const {
  // The ledger is a value map (target -> source), so a pointer-to-view
  // would dangle as soon as the map is destroyed. Returning nullptr keeps
  // the interface honest; callers that need the info use list_deployed()
  // or current_ledger() instead.
  (void)target;
  return nullptr;
}

std::vector<DeployedFileInfo> Direct::list_deployed() const {
  auto ledger = engine::load_deploy_ledger(config_.ledger_file);
  std::vector<DeployedFileInfo> result;
  result.reserve(ledger.size());
  const auto prefix_root = config_.game_dir / config_.deploy_prefix;
  for (const auto &[target, source] : ledger) {
    DeployedFileInfo info;
    info.target = target;
    info.source = source;
    // Derive mod_id from the deploy_prefix path structure
    // (game_dir/deploy_prefix/[mod_id/]file -> mod_id) when the deploy
    // layout includes it.
    if (config_.deploy_include_mod_id) {
      const auto rel = target.lexically_relative(prefix_root);
      if (!rel.empty() && *rel.begin() != "..")
        info.mod_id = rel.begin()->string();
    }
    // An original was backed up when a real file exists at
    // backup_root/<relative path> for this target.
    if (!config_.backup_root.empty()) {
      const auto rel = target.lexically_relative(config_.game_dir);
      if (!rel.empty() && *rel.begin() != "..") {
        std::error_code ec;
        info.backed_up =
            std::filesystem::is_regular_file(config_.backup_root / rel, ec) &&
            !ec;
      }
    }
    result.push_back(std::move(info));
  }
  return result;
}

std::map<std::filesystem::path, std::filesystem::path>
Direct::current_ledger() const {
  return engine::load_deploy_ledger(config_.ledger_file);
}

} // namespace Deploy
