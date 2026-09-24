#pragma once

// collectionRevision query: fetch one Nexus collection revision by slug +
// revision number. Field selection mirrors what the install path needs
// (modFiles with file identity + update policy, external resources) and
// matches node-nexus-api's IRevision / ICollectionRevisionMod shapes.
// Keyed by slug + revision like node-nexus-api getCollectionRevisionGraph.
// Qt-free.

#include <string>
#include <vector>

namespace engine::nexus_v2 {

class Client;

// One Nexus-hosted mod pinned by a collection revision. mod_id / file_name
// come from the nested file object and stay 0/empty when the file was
// removed server-side (file: null) - callers must tolerate that.
struct CollectionModFile {
  long long mod_id  = 0;
  long long file_id = 0;
  long long game_id = 0;
  std::string version;
  std::string file_name;
  std::string update_policy;  // "exact" | "latest" | "prefer"
  bool optional = false;
};

// Non-Nexus download (Google Drive, Mega, ...) bundled with the revision.
struct ExternalResource {
  std::string name;
  std::string url;
  std::string version;
  bool optional = false;
};

struct CollectionRevision {
  bool found                = false;
  long long collection_id   = 0;
  long long revision_number = 0;
  std::string revision_status;
  std::string slug;
  std::string name;
  std::string game_domain;  // collection.game.domainName
  std::vector<CollectionModFile> mods;
  std::vector<ExternalResource> external_resources;
};

struct FetchResult {
  bool ok = false;
  CollectionRevision revision;
  std::string error;
};

// Fixed query text: collectionRevision(slug, revision?, viewAdultContent).
// revision 0 in variables means "latest" (key omitted, server default).
std::string build_collection_revision_query();

// variables JSON for the query above. revision <= 0 omits the key.
std::string build_collection_revision_variables(const std::string &slug,
                                                long long revision);

// Parse data.collectionRevision (already extracted by Client::parse_response).
// Returns false with out_error set when the node is missing/unparseable;
// individual mod entries are skipped, never fatal.
bool parse_collection_revision_data(const std::string &data_json,
                                    CollectionRevision &out, std::string &out_error);

// Convenience: query + parse in one call. Never throws.
FetchResult fetch_collection_revision(Client &client, const std::string &slug,
                                      long long revision);

}  // namespace engine::nexus_v2
