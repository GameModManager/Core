#pragma once

#include "engine/deploy/interface.h"

#include <filesystem>
#include <memory>
#include <string>

namespace Deploy {

// Strategy-selection factory: creates the appropriate Deploy::Interface
// implementation from a strategy name string.  Absorbs the conditional
// logic that was previously duplicated in settings_controller.cpp
// (UI deploy setup) and instance_utils.cpp (launch-time deploy).
//
// The factory is the single source of truth for mapping a strategy name
// (e.g. "symlink", "overlayfs", "direct") to a concrete strategy object.
// Platform-specific capability checks (OverlayFS support) are handled
// internally so callers don't need #ifdef blocks.
class Core {
public:
  // Create a strategy object for the given name.
  //
  // Recognized names (case-sensitive):
  //   "overlayfs" - OverlayFsDeploy (Linux only; falls back to Symlink
  //                  when the platform does not support overlayfs).
  //   "direct"    - Symlink (same on-disk result as the default, but
  //                  routed through the Direct lifecycle object in
  //                  callers that need deploy_all/undeploy/sync).
  //   "symlink"   - Symlink (the default).
  //   ""          - Symlink (empty = default).
  //
  // The caller owns the returned strategy.  Returns nullptr only if
  // `name` is an unrecognized non-empty string (defensive; callers
  // should validate against the known set).
  //
  // Parameters:
  //   name            - strategy name from instance.toml / game knowledge.
  //   case_sensitive  - forwarded to Symlink / OverlayFsDeploy constructors.
  //   staging_dir     - staging root for OverlayFsDeploy (only used when
  //                     name == "overlayfs" and the platform supports it).
  //                     When empty and overlayfs is requested, the factory
  //                     uses a default under the process's current dir
  //                     (callers should always provide this).
  [[nodiscard]] static std::unique_ptr<Interface>
  create(const std::string &name, bool case_sensitive = true,
         const std::filesystem::path &staging_dir = {});

  // Human-readable label for the strategy that create() would return for
  // the given name.  Useful for log messages ("Deploy strategy: Symlink").
  [[nodiscard]] static const char *label(const std::string &name);

  // True when the platform supports OverlayFS deploys.  The factory checks
  // this internally, but callers that need to decide staging-dir setup
  // (e.g. settings_controller) can query it without duplicating the
  // platform ifdef.
  [[nodiscard]] static bool
  overlay_supported(const std::filesystem::path &overwrite_dir = {});
};

} // namespace Deploy
