#include "engine/game/registry/capability_manifest.h"
#include <string>

namespace engine {

namespace {
  // Order: data caps first, then functional. Append-only; never renumber.
  constexpr CapabilityManifestEntry kManifest[] = {
      // -- DATA capabilities (TOML-declarable, NULL invoke legal) --
      {"layout.mods", CapabilityKind::Data, 1},
      {"layout.install", CapabilityKind::Data, 1},
      {"game.native_files", CapabilityKind::Data, 1},
      {"deploy.policy", CapabilityKind::Data, 1},
      {"load_order.encoding", CapabilityKind::Data, 1},
      {"saves.list", CapabilityKind::Data, 1},
      {"plugins.database", CapabilityKind::Data, 1},
      {"archives", CapabilityKind::Data, 1},
      {"conflicts.scope", CapabilityKind::Data, 1},
      {"catalog.icon", CapabilityKind::Data, 1},
      {"catalog.masterlist", CapabilityKind::Data, 1},
      {"catalog.sources", CapabilityKind::Data, 1},
      {"ui.tabs", CapabilityKind::Data, 1},
      // -- FUNCTIONAL capabilities (code only, invoke required) --
      {"saves.parse", CapabilityKind::Functional, 1},
      {"saves.overlay", CapabilityKind::Functional, 1},
      {"preview.file", CapabilityKind::Functional, 1},
      {"animation.parse", CapabilityKind::Functional, 1},
      {"sort.provider", CapabilityKind::Functional, 1},
      {"tool", CapabilityKind::Functional, 1},
      {"installer.stage", CapabilityKind::Functional, 1},
      {"detection.validator", CapabilityKind::Functional, 1},
      {"event.subscribe", CapabilityKind::Functional, 1},
      {"ui.diagnostics", CapabilityKind::Functional, 1},
      {"ui.settings", CapabilityKind::Functional, 1},
      {"install.checker", CapabilityKind::Functional, 1},
      {"plugin.settings", CapabilityKind::Functional, 1},
  };
}  // namespace

const CapabilityManifestEntry *find_capability(const char *id) {
  if (!id)
    return nullptr;
  for (const auto &e : kManifest) {
    if (std::string(e.id) == id)
      return &e;
  }
  return nullptr;
}

}  // namespace engine
