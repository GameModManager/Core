#pragma once

// Source-agnostic collection resolver.
//
// Converts a Collection::Manifest into pipeline-ready engine::Mod objects,
// grouped by phase and validated against the manifest's own rules. This is
// the bridge between the manifest schema (pure data) and the install
// pipeline (which consumes engine::Mod one at a time).
//
// The resolver does NOT perform network I/O or schema validation. It
// assumes the manifest is structurally valid (callers run archive
// integrity + schema validation first). Its job is purely data
// transformation + rule diagnostics.
//
// Engine layer - Qt-free.

#include "engine/collection/manifest.h"
#include "engine/mod/model/mod.h"

#include <string>
#include <vector>

namespace engine::Collection {

// ---------------------------------------------------------------------------
// Resolved mod
// ---------------------------------------------------------------------------

// A single mod resolved from the manifest: the pipeline-ready engine::Mod
// plus metadata needed by the batch installer and diagnostics.
struct ResolvedMod {
  ::engine::Mod mod;                // ready for Pipeline::run()
  const ModEntry *entry = nullptr;  // back-reference to manifest entry
  bool resolvable       = true;     // false = source couldn't be mapped
  std::string error;                // human-readable reason when !resolvable
};

// ---------------------------------------------------------------------------
// Rule diagnostic
// ---------------------------------------------------------------------------

// A warning or error from rule validation.
struct RuleDiagnostic {
  enum class Severity { Warning, Error };

  Severity severity = Severity::Warning;
  std::string message;    // human-readable description
  std::string rule_from;  // offending rule's "from" id
  std::string rule_to;    // offending rule's "to" id
};

// ---------------------------------------------------------------------------
// Resolved collection
// ---------------------------------------------------------------------------

// The full result of resolving a manifest: all mods in pipeline order,
// grouped by phase, plus diagnostics.
struct ResolvedCollection {
  std::vector<ResolvedMod> mods;

  // Mods grouped by install phase: phases[0] installs first, then
  // phases[1], etc. Each inner vector preserves the manifest's
  // original order within that phase (which is the file-priority
  // order for conflict resolution).
  std::vector<std::vector<ResolvedMod *>> phases;

  // Rule validation diagnostics (warnings for missing dependencies,
  // circular requires, etc.).
  std::vector<RuleDiagnostic> diagnostics;
};

// ---------------------------------------------------------------------------
// Resolver
// ---------------------------------------------------------------------------

// Stateless resolver. Converts a Manifest into a ResolvedCollection.
class Resolver {
public:
  // Resolve an entire manifest into pipeline-ready Mod objects.
  //
  // Steps:
  //   1. Walk mods[], map each ModSource variant to engine::Mod fields
  //   2. Group by phase (stable sort preserving manifest order)
  //   3. Validate rules (before/after/requires/conflicts) and emit
  //      diagnostics for missing references or cycles
  //
  // Mods with unknown provider types get resolvable=false and a
  // diagnostic; the batch installer handles them as "skip with warning"
  // per the format spec.
  ResolvedCollection resolve(const Manifest &manifest) const;

  // Convert a single ModSource variant to engine::Mod download metadata.
  // Returns false if the source type is unknown/unrecognized.
  // When false is returned, the mod's download fields are left empty and
  // the caller should set resolvable=false.
  static bool populate_mod_source(const ModSource &source, ::engine::Mod &mod);

private:
  // Validate rules against the resolved mod set.
  static std::vector<RuleDiagnostic>
  validate_rules(const std::vector<ResolvedMod> &mods, const std::vector<Rule> &rules);
};

}  // namespace engine::Collection
