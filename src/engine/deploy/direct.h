#pragma once

#include "engine/deploy/deploy_utils.h"
#include "engine/deploy/interface.h"
#include "engine/deploy/symlink.h"

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace Deploy {

// Result of a sync operation: what changed.
struct SyncResult {
  int files_deployed = 0;  // new/re-pointed files
  int files_removed = 0;   // stale files unlinked
  int files_unchanged = 0; // already correct
  int files_failed = 0;    // -1 sentinel: deploy_all reported a failure
};

// Status of a single deployed file.
struct DeployedFileInfo {
  std::filesystem::path target; // path in game_dir
  std::filesystem::path source; // mod folder source
  std::string mod_id;           // owning mod (empty when not derivable)
  bool backed_up = false;       // original game file was backed up
};

// Direct-deploy strategy: deploys mods straight into game_dir using symlinks
// (via Symlink internally), with a persistent ledger for O(Δ)
// incremental redeploys and original-file backup/restore.
//
// This is a strategy that owns the full lifecycle:
//   - deploy_all(): full deploy of all enabled mods
//   - undeploy():   remove all deployed files + restore originals
//   - sync():       incremental re-deploy of only changed files
//   - deploy() [inherited]: per-file symlink deploy (delegates to Symlink)
//   - remove() [inherited]: per-file symlink removal (delegates to Symlink)
//
// Thread safety: methods are safe to call from any thread, but concurrent
// sync()/deploy_all()/undeploy() calls are NOT supported (ledger consistency).
class Direct : public Interface {
public:
  struct Config {
    std::filesystem::path mods_dir;
    std::filesystem::path game_dir;
    std::string deploy_prefix;
    bool deploy_include_mod_id = false;
    std::string disable_mechanism;
    bool case_sensitive = true;
    std::filesystem::path ledger_file;
    std::filesystem::path backup_root; // Original_Files dir
  };

  explicit Direct(Config config);

  // --- Interface (per-file, inherited) ---

  bool deploy(const std::filesystem::path &source,
              const std::filesystem::path &target) override;

  bool remove(const std::filesystem::path &target) override;

  // --- Direct-deploy lifecycle methods ---

  // Full deploy: walks all enabled mods, resolves winners, diffs against
  // the ledger, links/removes in parallel. This is the O(N) first-deploy
  // path.
  // progress: callback (files_done, files_total), total==0 means nothing
  // to do.
  [[nodiscard]] bool deploy_all(const engine::DeployProgressFn &progress = {});

  // Undeploy: removes all files from game_dir that the ledger says were
  // deployed, restores backed-up originals, drops the ledger.
  // This is the "remove deployed files" action.
  [[nodiscard]] bool undeploy(const engine::DeployProgressFn &progress = {});

  // Incremental sync: re-evaluates winners, deploys only what changed.
  // Returns a SyncResult with counts of what happened.
  // This is the O(Δ) path — same as deploy_all but fast when most files
  // are unchanged (stat-only for unchanged entries).
  [[nodiscard]] SyncResult sync(const engine::DeployProgressFn &progress = {});

  // Query: is a specific path currently deployed by us?
  [[nodiscard]] bool is_deployed(const std::filesystem::path &target) const;

  // Query: get the source mod for a deployed target.
  // NOTE: returns nullptr in the initial implementation — the ledger is a
  // value map, so a pointer-to-view would dangle. Callers should use
  // list_deployed()/current_ledger() instead.
  [[nodiscard]] const DeployedFileInfo *
  find(const std::filesystem::path &target) const;

  // Query: list all currently deployed files.
  [[nodiscard]] std::vector<DeployedFileInfo> list_deployed() const;

  // Query: load the raw ledger map (for external consumers like the
  // stray-plugin scan).
  [[nodiscard]] std::map<std::filesystem::path, std::filesystem::path>
  current_ledger() const;

  // Access the underlying per-file strategy (for callers that need
  // Symlink specifically).
  [[nodiscard]] const Symlink &file_strategy() const { return file_strategy_; }

  // Access the config (read-only).
  [[nodiscard]] const Config &config() const { return config_; }

private:
  Config config_;
  Symlink file_strategy_; // per-file symlink/copy operations
};

} // namespace Deploy
