#pragma once

// Source-agnostic CollectionProvider interface.
//
// Abstracts how collections are fetched regardless of source: Nexus
// collections, .gmmpack archives, manual imports, etc. Each source type
// implements this interface; consumers (batch installer, import UI) work
// only through it.
//
// This is the *collection* equivalent of Source::Interface (which handles
// individual mod downloads). A CollectionProvider fetches a manifest; a
// Resolver converts it to pipeline-ready Mod objects.
//
// Engine layer - Qt-free.

#include "engine/collection/manifest.h"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace engine::Collection {

// ---------------------------------------------------------------------------
// Fetch result
// ---------------------------------------------------------------------------

// A successfully fetched manifest plus the source_id it was fetched from.
struct FetchResult {
  Manifest manifest;
  std::string source_id;  // original identifier (URL, path, etc.)
};

// Error returned when a fetch fails.
struct FetchError {
  std::string message;  // human-readable reason
  int http_status = 0;  // HTTP status code (0 if not applicable)
};

// Result of a collection fetch: either a valid FetchResult or a FetchError.
using FetchOutcome = std::variant<FetchResult, FetchError>;

// ---------------------------------------------------------------------------
// Provider interface
// ---------------------------------------------------------------------------

// Abstract interface for collection sources.
//
// Each source type (Nexus collections, .gmmpack, manual import, future
// providers) implements this. The interface is intentionally narrow:
// fetch a manifest by source identifier. Resolution (converting the
// manifest's ModSource entries into pipeline-ready Mod objects) is
// handled by Resolver, not by the provider, because resolution logic
// is source-agnostic.
class Provider {
public:
  virtual ~Provider() = default;

  // Source type identifier (e.g. "nexus_collection", "gmmpack", "manual_import").
  // Matches the string used in Source::Registry for mod-file providers when
  // the same string is appropriate, but a collection source may use a
  // different string than its underlying mod-file provider.
  virtual std::string source_type() const = 0;

  // Human-readable name for the UI (e.g. "Nexus Collections").
  virtual std::string display_name() const = 0;

  // Fetch a collection manifest from the source.
  //
  // `source_id` is source-specific:
  //   - Nexus: collection URL or numeric ID
  //   - gmmpack: file path or URL to the .gmmpack archive
  //   - manual_import: directory path containing manifest.json
  //
  // Returns FetchResult on success, FetchError on failure.
  // The provider does NOT validate the manifest schema here; callers
  // should use the archive integrity / schema validation pipeline
  // before acting on the result.
  virtual FetchOutcome fetch(const std::string &source_id) = 0;

  // Whether this provider can handle the given source identifier.
  // Used by the provider registry to route source IDs to the right
  // provider. Default implementation returns false (providers must
  // opt in).
  virtual bool can_handle(const std::string &source_id) const { return false; }
};

}  // namespace engine::Collection
