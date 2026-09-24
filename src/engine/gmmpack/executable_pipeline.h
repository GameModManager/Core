#pragma once

// Generic pipeline for pack-declared executables (gmmpack executables/*.json).
//
// Two roles:
//   setup    - one-shot generators run during install/update when
//              auto_run is set (output captured as a synthetic mod or
//              left in place). Never run interactively.
//   launcher - designates the pack's Play button exe. Never run during
//              install; converted to LaunchParams for the launch path.
//
// Sequencing between executables (and the mods they depend on) reuses the
// manifest rules[] over the shared mod+executable id-space. Process
// mechanics (fork+exec, stdout/stderr capture) reuse run_captured; the
// launcher role reuses LaunchParams. Qt-free.

#include <filesystem>
#include <string>
#include <vector>

#include "engine/core/util/process_utils.h"
#include "engine/deploy/launch/launcher.h"
#include "engine/gmmpack/types.h"

namespace engine::gmmpack {

// OS key used by platform.<os> blocks. Compile-time host value.
std::string host_os_name();  // "linux" | "macos" | "windows"

// Executable with per-instance absolute paths resolved. Pure path math
// against mods_root/<sourceModId>/... - no filesystem access, so missing
// mods simply resolve to paths that fail later at run time.
struct ResolvedExecutable {
  ExecutableEntry entry;
  std::filesystem::path executable_path;
  std::filesystem::path working_dir;     // exe parent dir when workingDir empty
  std::filesystem::path output_dir;      // empty when no output.path declared
  std::vector<std::string> argv;         // [exe, ...arguments, [argName out]]?
  std::vector<std::string> environment;  // merged base + per-OS, "K=V"
};

// Base envVars + platform.<os>.envVars (additive, OS wins on conflict),
// returned as sorted "K=V" entries for deterministic tests/launches.
std::vector<std::string> merged_environment(const ExecutableEntry &entry,
                                            const std::string &os);

// Resolve every pack executable against the installed-mods root.
std::vector<ResolvedExecutable>
resolve_executables(const Gmmpack &pack, const std::filesystem::path &mods_root,
                    const std::string &os = host_os_name());

// Topological order over the shared mod+executable id-space honoring
// rules[]: before (from first), after/requires (to/dependency first).
// Unknown ids and conflicts contribute no edges. Unconstrained ids keep
// pack order. has_cycle is set when rules contradict; order then falls
// back to pack order for the cyclic remainder.
struct RunOrder {
  std::vector<std::string> ids;
  bool has_cycle = false;
};

RunOrder order_for_run(const std::vector<std::string> &ids,
                       const std::vector<ManifestRule> &rules);

// Setup executables due to auto-run, in run order. Launcher-role entries
// are never included - registering them as Play exes is not running them.
std::vector<std::string> setup_auto_run_ids(const Gmmpack &pack, const RunOrder &order);

// Stable (cross-run, cross-platform) hash of an executable's modset inputs.
// Inputs are "modId:version"-style tokens; sorted internally so callers
// need not pre-sort. FNV-1a: change detection only, not cryptographic.
std::string modset_input_hash(const std::vector<std::string> &inputs);

// True when a setup executable must re-fire: opted into rerunOnModsetChange
// and the current inputs differ from the last run's recorded hash. A first
// install (empty last hash) is covered by setup_auto_run_ids, not here.
bool exe_rerun_due(const ExecutableEntry &entry, const std::string &current_hash,
                   const std::string &last_hash);

// Run a resolved setup executable synchronously, capturing stdout/stderr.
// Environment and cwd come from the resolved entry. Reuses run_captured.
CapturedProcess run_executable(const ResolvedExecutable &resolved);

// Capture a finished run's output. inPlace (or no output block) is a no-op
// success. syntheticMod replaces mods_root/<syntheticModId> with the output
// tree - derived output is fully replaced, never merged.
struct CaptureResult {
  bool ok = false;
  std::string error;
};

CaptureResult capture_exe_output(const ResolvedExecutable &resolved,
                                 const std::filesystem::path &mods_root);

// Convert a launcher-role executable into launch parameters for the Play
// button path (executable, args, environment, cwd). No process is started.
LaunchParams to_launch_params(const ResolvedExecutable &resolved);

}  // namespace engine::gmmpack
