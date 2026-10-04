#pragma once

// Nexus collection adapter: CollectionProvider over Nexus collections.
//
// Two fetch paths, both producing the source-agnostic Collection::Manifest:
//   - collection.json files (Vortex export) via the existing Nexus::parse.
//   - live revisions via the v2 GraphQL client (slug[@revision] or URL).
//
// Revision mods become SourceNexus entries (the same "nexus" provider the
// Resolver maps to engine::Mod download fields); external resources become
// SourceDirect browser entries. Mods whose file was removed upstream
// (file: null, modId unknown) are skipped with a diagnostic, never fatal.
// Premium-vs-free routing reuses Collection::route_download with the stored
// Nexus account status (key = authenticated, premium = auto-download).
//
// Engine layer - Qt-free.

#include "engine/collection/download_router.h"
#include "engine/collection/manifest.h"
#include "engine/collection/provider.h"
#include "engine/network/nexus_v2/collection_revision.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace engine::Collection::Nexus {

// ---------------------------------------------------------------------------
// Source id parsing
// ---------------------------------------------------------------------------

// Parsed form of a Provider::fetch source_id.
struct CollectionRef {
  std::string slug;            // collection slug for GraphQL (empty for files)
  long long revision = 0;      // 0 = latest
  bool is_file       = false;  // source_id is a collection.json path
  std::string file_path;
};

// Parse "slug[@revision]", a nexusmods.com collections URL, an
// "nxm://<game>/collections/<id>[/revisions/<n>]" link, or a .json path.
// Never throws; an id in none of those shapes - including an nxm:// mod link,
// which is not a collection - yields an empty slug, non-file ref (can_handle
// rejects those before fetch, and fetch names the accepted forms).
CollectionRef parse_source_id(const std::string &source_id);

// ---------------------------------------------------------------------------
// Revision -> manifest conversion (pure, no I/O)
// ---------------------------------------------------------------------------

// One skipped revision entry + why. Fetch continues past skips.
struct SkipDiagnostic {
  std::string mod_label;  // file name or "file_id <n>"
  std::string reason;
};

struct RevisionManifest {
  Manifest manifest;
  std::vector<SkipDiagnostic> skipped;
  // Nexus's own revisionStatus ("published", "retracted", "discarded", ...).
  // Empty when the gateway reported none. Not fatal either way - a retracted
  // revision still has a mod list, the user just has to know that is what
  // they are looking at.
  std::string revision_status;
};

// Map one revision mod to a SourceNexus. nullopt when unresolvable
// (file: null server-side, modId unknown) - caller skips with a diagnostic.
std::optional<SourceNexus> mod_file_to_source(const nexus_v2::CollectionModFile &mod,
                                              const std::string &game_domain);

// Convert a fetched GraphQL revision to a Manifest, skipping unresolvable
// entries (null files, empty external URLs) with diagnostics.
RevisionManifest revision_to_manifest(const nexus_v2::CollectionRevision &rev);

// ---------------------------------------------------------------------------
// Download path (premium vs free)
// ---------------------------------------------------------------------------

// Account status from the stored Nexus credentials: authenticated = API key
// present, can_auto_download = premium tier. Matches the mapping documented
// in download_router.h.
AccountStatus account_status();

// Route one manifest source through the generic router with the current
// Nexus account status (premium = Auto, free/anonymous = Browser).
RouteOutcome route_download(const ModSource &source);

// ---------------------------------------------------------------------------
// Provider
// ---------------------------------------------------------------------------

class Adapter : public Provider {
public:
  using RevisionFetcher =
      std::function<nexus_v2::FetchResult(const std::string &slug, long long revision)>;

  // Test seam: inject a fake fetcher (no network). Reads metadata only - no
  // collection.json reader is attached, so the archive is never requested and
  // nothing is reported about it.
  explicit Adapter(RevisionFetcher fetcher);

  // Test seam: inject a fake collection.json reader (no network).
  using JsonFetcher = std::function<bool(
      const std::string &download_link, std::string &out_json, std::string &out_error)>;
  Adapter(RevisionFetcher fetcher, JsonFetcher json_fetcher);

  // Production wiring: fetcher over network::instance() + stored API key.
  Adapter();

  std::string source_type() const override { return "nexus_collection"; }
  std::string display_name() const override { return "Nexus Collections"; }

  // File ids go through Nexus::parse_file; slugs/URLs through the v2
  // client + revision_to_manifest, then the revision's own .zip is read and
  // merged over that. Per-mod skips land in last_skipped().
  FetchOutcome fetch(const std::string &source_id) override;

  // .json paths, nexusmods.com collection URLs, nxm:// collection links, bare
  // slugs. An nxm:// mod link is not a collection and is not handled here.
  bool can_handle(const std::string &source_id) const override;

  // Diagnostics from the last fetch() (empty after file fetches).
  const std::vector<SkipDiagnostic> &last_skipped() const { return last_skipped_; }

  // Nexus's revisionStatus for the last fetched revision, empty for a
  // collection.json (which has no revision) or when Nexus reported none.
  const std::string &last_revision_status() const { return last_revision_status_; }

  // Everything the last fetch() could not read, including the archive itself
  // when it could not be fetched. Never fatal: the import proceeds on metadata
  // alone and the caller reports these.
  const std::vector<Unresolved> &last_unresolved() const { return last_unresolved_; }

private:
  RevisionFetcher fetcher_;
  JsonFetcher json_fetcher_;
  std::vector<SkipDiagnostic> last_skipped_;
  std::string last_revision_status_;
  std::vector<Unresolved> last_unresolved_;
};

}  // namespace engine::Collection::Nexus
