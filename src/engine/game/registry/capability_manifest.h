#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace engine {

// v3 P4 - capability manifest. The single source of truth for which
// capability ids exist, whether they are data-only (declarable from a TOML
// descriptor, NULL invoke legal) or functional (code only, invoke required),
// and the payload/method-set version the host understands.
//
// The dispatch table (attach_capability) feeds the EXISTING registries from
// this manifest; it never creates a parallel store.

enum class CapabilityKind { Data, Functional };

struct CapabilityManifestEntry {
  const char *id;
  CapabilityKind kind;
  // Method-set version the host understands for this id. A capability whose
  // declared version is newer than this is dropped with a warning.
  std::uint32_t payload_version;
};

// Look up a capability id in the manifest. Returns nullptr for unknown ids.
const CapabilityManifestEntry *find_capability(const char *id);

inline bool is_known_capability(const char *id) {
  return find_capability(id) != nullptr;
}

// True when a NULL invoke is legal for this id (data-only capabilities).
inline bool capability_allows_null_invoke(const char *id) {
  const auto *e = find_capability(id);
  return e != nullptr && e->kind == CapabilityKind::Data;
}

// Convenience: kind of a known id; defaults to Functional for unknown.
inline CapabilityKind capability_kind(const char *id) {
  const auto *e = find_capability(id);
  return e ? e->kind : CapabilityKind::Functional;
}

}  // namespace engine
