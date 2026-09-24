#include "engine/network/nexus_v2/collection_revision.h"

#include "engine/network/nexus_v2/client.h"

#include <nlohmann/json.hpp>

namespace engine::nexus_v2 {

namespace {

  // Tolerant getters: the gateway may add/drop fields at any time.
  long long as_int(const nlohmann::json &j, const char *key) {
    const auto it = j.find(key);
    if (it == j.end() || it->is_null())
      return 0;
    if (it->is_number())
      return it->get<long long>();
    if (it->is_string()) {
      try {
        return std::stoll(it->get<std::string>());
      } catch (const std::exception &) {
        return 0;
      }
    }
    return 0;
  }

  std::string as_string(const nlohmann::json &j, const char *key) {
    const auto it = j.find(key);
    if (it == j.end() || it->is_null())
      return {};
    if (it->is_string())
      return it->get<std::string>();
    if (it->is_number())
      return std::to_string(it->get<long long>());
    return {};
  }

  bool as_bool(const nlohmann::json &j, const char *key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_boolean() && it->get<bool>();
  }

}  // namespace

std::string build_collection_revision_query() {
  return "query collectionRevision($slug: String!, $revision: Int, $viewAdultContent: "
         "Boolean) {"
         " collectionRevision(slug: $slug, revision: $revision, viewAdultContent: "
         "$viewAdultContent) {"
         " id revisionNumber revisionStatus"
         " collection { id name slug game { domainName } }"
         " modFiles { gameId fileId version optional updatePolicy"
         " file { modId fileId name version size } }"
         " externalResources { name resourceUrl version optional } } }";
}

std::string build_collection_revision_variables(const std::string &slug,
                                                long long revision) {
  nlohmann::json vars;
  vars["slug"] = slug;
  if (revision > 0)
    vars["revision"] = revision;
  vars["viewAdultContent"] = true;
  return vars.dump();
}

bool parse_collection_revision_data(const std::string &data_json,
                                    CollectionRevision &out, std::string &out_error) {
  nlohmann::json node;
  try {
    node = nlohmann::json::parse(data_json);
  } catch (const std::exception &e) {
    out_error = std::string("invalid collectionRevision node: ") + e.what();
    return false;
  }
  if (node.is_null() || !node.is_object()) {
    out_error = "collection not found";
    return false;
  }

  CollectionRevision rev;
  rev.found           = true;
  rev.revision_number = as_int(node, "revisionNumber");
  rev.revision_status = as_string(node, "revisionStatus");

  const auto coll_it = node.find("collection");
  if (coll_it != node.end() && coll_it->is_object()) {
    rev.collection_id  = as_int(*coll_it, "id");
    rev.name           = as_string(*coll_it, "name");
    rev.slug           = as_string(*coll_it, "slug");
    const auto game_it = coll_it->find("game");
    if (game_it != coll_it->end() && game_it->is_object())
      rev.game_domain = as_string(*game_it, "domainName");
  }

  const auto mods_it = node.find("modFiles");
  if (mods_it != node.end() && mods_it->is_array()) {
    for (const auto &m : *mods_it) {
      if (!m.is_object())
        continue;
      CollectionModFile entry;
      entry.game_id       = as_int(m, "gameId");
      entry.file_id       = as_int(m, "fileId");
      entry.version       = as_string(m, "version");
      entry.update_policy = as_string(m, "updatePolicy");
      entry.optional      = as_bool(m, "optional");
      const auto file_it  = m.find("file");
      if (file_it != m.end() && file_it->is_object()) {
        entry.mod_id    = as_int(*file_it, "modId");
        entry.file_name = as_string(*file_it, "name");
        if (entry.file_id == 0)
          entry.file_id = as_int(*file_it, "fileId");
      }
      if (entry.file_id == 0 && entry.game_id == 0)
        continue;
      rev.mods.push_back(std::move(entry));
    }
  }

  const auto ext_it = node.find("externalResources");
  if (ext_it != node.end() && ext_it->is_array()) {
    for (const auto &e : *ext_it) {
      if (!e.is_object())
        continue;
      ExternalResource res;
      res.name     = as_string(e, "name");
      res.url      = as_string(e, "resourceUrl");
      res.version  = as_string(e, "version");
      res.optional = as_bool(e, "optional");
      if (res.url.empty())
        continue;
      rev.external_resources.push_back(std::move(res));
    }
  }

  out = std::move(rev);
  return true;
}

FetchResult fetch_collection_revision(Client &client, const std::string &slug,
                                      long long revision) {
  FetchResult result;
  GraphqlResult gql =
      client.query("collectionRevision", build_collection_revision_query(),
                   build_collection_revision_variables(slug, revision));
  if (!gql.ok) {
    result.error = gql.error;
    return result;
  }
  result.ok = parse_collection_revision_data(gql.data, result.revision, result.error);
  return result;
}

}  // namespace engine::nexus_v2
