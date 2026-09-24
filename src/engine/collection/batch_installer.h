#pragma once

// Source-agnostic batch install driver for collections / modpacks.
//
// Iterates a Resolver-produced ResolvedCollection phase by phase (phase 0
// first, manifest order within each phase) and runs each entry through the
// existing single-mod install pipeline. Works with any manifest conforming
// to the Collection::Manifest schema - Nexus collections arrive as
// collection.json data through the nxm:// API, .gmmpack archives and manual
// imports produce the same schema; this driver never sees the difference.
//
// Behavior per entry:
//   - unresolvable entries (unknown source provider) are skipped with a
//     warning, never aborting the batch (format spec: warn, don't abort)
//   - entries whose expected archive hash matches the installed mod's
//     recorded hash are skipped as up-to-date
//   - install failures are recorded per-mod and the batch continues
//   - user cancel (PipelineResult::Canceled) stops the batch; entries never
//     reached are reported as NotAttempted
//
// The single-mod pipeline is injected as a callable so this stays decoupled
// from pipeline construction and UI threading: production wires
// Pipeline::run, tests inject fakes.
//
// Engine layer - Qt-free.

#include "engine/collection/resolver.h"
#include "engine/pipeline/pipeline.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace engine::Collection {

// ---------------------------------------------------------------------------
// Per-mod verdict
// ---------------------------------------------------------------------------

enum class ModVerdict {
  Installed,            // pipeline ran and reported Success
  SkippedUpToDate,      // installed hash matches the manifest's expected hash
  SkippedUnresolvable,  // source couldn't be mapped to a Mod (warn, don't abort)
  Failed,               // pipeline ran and reported Failed
  Canceled,             // pipeline reported Canceled (user abort, batch stops)
  NotAttempted,         // batch stopped before reaching this entry
};

// One entry per manifest mod, in install (phase) order.
struct ModInstallResult {
  std::string mod_id;
  ModVerdict verdict = ModVerdict::NotAttempted;
  std::string message;  // failure/skip reason, empty on clean install
};

// ---------------------------------------------------------------------------
// Batch result
// ---------------------------------------------------------------------------

struct BatchResult {
  std::vector<ModInstallResult> mods;  // one per manifest entry, phase order
  int installed            = 0;
  int skipped_up_to_date   = 0;
  int skipped_unresolvable = 0;
  int failed               = 0;
  bool canceled            = false;  // true when a user cancel stopped the batch early
};

// ---------------------------------------------------------------------------
// Dependencies (injected)
// ---------------------------------------------------------------------------

struct BatchInstallerDeps {
  // Runs the existing single-mod install pipeline for one resolved mod.
  // Production wires Pipeline::run; unset = every mod fails with
  // "no installer wired" (batch still runs to completion).
  std::function<PipelineResult(::engine::Mod &)> install_one;

  // Returns the installed mod's recorded archive hash for mod_id, or
  // nullopt when that mod is not installed. Unset = no up-to-date check,
  // everything installs. Compared against expected_hash() below.
  std::function<std::optional<std::string>(const std::string &mod_id)>
      installed_hash_for;

  // Progress after each entry: processed count, total entries, mod id.
  // Unset = no reporting.
  std::function<void(int done, int total, const std::string &mod_id)> on_progress;
};

// ---------------------------------------------------------------------------
// BatchInstaller
// ---------------------------------------------------------------------------

class BatchInstaller {
public:
  explicit BatchInstaller(BatchInstallerDeps deps);

  // Install every entry in collection, phase by phase. Never throws for
  // per-mod conditions; all outcomes land in the returned BatchResult.
  BatchResult install(const ResolvedCollection &collection) const;

private:
  BatchInstallerDeps deps_;
};

// Expected archive hash for a manifest mod source, with an optional
// "sha256:" prefix stripped. nullopt when the source carries no hash
// (empty field, or Steam Workshop whose version is always unknown ahead
// of time). NOTE: sources record whatever digest the provider gave them
// (Nexus file metadata is md5); callers must compare against the same
// algorithm on the installed side - this only normalizes the prefix.
std::optional<std::string> expected_hash(const ModSource &source);

}  // namespace engine::Collection
