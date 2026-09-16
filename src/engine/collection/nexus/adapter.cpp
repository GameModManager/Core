#include "engine/collection/nexus/adapter.h"

#include "engine/collection/nexus/parser.h"
#include "engine/network/network_manager.h"
#include "engine/network/nexus_v2/client.h"
#include "engine/network/nexus_v2/premium.h"
#include "engine/source/nexus/auth.h"

#include <cctype>

namespace engine::Collection::Nexus {

namespace {

// ".json" suffix (case-sensitive, matches the Vortex export name).
bool is_json_path(const std::string& id) {
  return id.size() > 5 && id.ends_with(".json");
}

bool is_nexus_collection_url(const std::string& id) {
  return id.find("nexusmods.com") != std::string::npos &&
         id.find("collection") != std::string::npos;
}

// Bare slug: alnum + '-' + '_' with optional "@<digits>" revision pin.
bool is_bare_slug(const std::string& id) {
  if (id.empty())
    return false;
  std::string slug = id;
  const auto at = slug.find('@');
  if (at != std::string::npos) {
    const std::string rev = slug.substr(at + 1);
    if (rev.empty())
      return false;
    for (unsigned char c : rev) {
      if (!std::isdigit(c))
        return false;
    }
    slug.resize(at);
  }
  if (slug.empty())
    return false;
  for (unsigned char c : slug) {
    if (!std::isalnum(c) && c != '-' && c != '_')
      return false;
  }
  return true;
}

// Path segment starting at pos up to the next '/' '?' or '#'.
std::string take_segment(const std::string& url, std::size_t pos) {
  const auto end = url.find_first_of("/?#", pos);
  return url.substr(pos, end == std::string::npos ? end : end - pos);
}

// "?revision=N" / "&revision=N" query param, 0 when absent/invalid.
long long query_revision(const std::string& url) {
  for (const char* key : {"?revision=", "&revision="}) {
    const auto pos = url.find(key);
    if (pos == std::string::npos)
      continue;
    const std::string val = take_segment(url, pos + std::string(key).size());
    if (val.empty())
      continue;
    try {
      const long long rev = std::stoll(val);
      if (rev > 0)
        return rev;
    } catch (const std::exception&) {
    }
  }
  return 0;
}

// ".../collections/<slug>[/revisions/<n>][?...]".
CollectionRef parse_url(const std::string& url) {
  CollectionRef ref;
  const auto coll = url.find("/collections/");
  if (coll == std::string::npos)
    return ref;
  ref.slug = take_segment(url, coll + std::string("/collections/").size());
  const auto revpos = url.find("/revisions/", coll);
  if (revpos != std::string::npos) {
    try {
      ref.revision = std::stoll(
          take_segment(url, revpos + std::string("/revisions/").size()));
    } catch (const std::exception&) {
      ref.revision = 0;
    }
  }
  if (ref.revision <= 0)
    ref.revision = query_revision(url);
  return ref;
}

Adapter::RevisionFetcher default_fetcher() {
  return [](const std::string& slug, long long revision) {
    nexus_v2::Client client(network::instance());
    return nexus_v2::fetch_collection_revision(client, slug, revision);
  };
}

}  // namespace

CollectionRef parse_source_id(const std::string& source_id) {
  CollectionRef ref;
  if (is_json_path(source_id)) {
    ref.is_file = true;
    ref.file_path = source_id;
    return ref;
  }
  if (is_nexus_collection_url(source_id))
    return parse_url(source_id);
  const auto at = source_id.find('@');
  if (at != std::string::npos) {
    ref.slug = source_id.substr(0, at);
    try {
      ref.revision = std::stoll(source_id.substr(at + 1));
    } catch (const std::exception&) {
      ref.revision = 0;
    }
  } else {
    ref.slug = source_id;
  }
  return ref;
}

std::optional<SourceNexus> mod_file_to_source(
    const nexus_v2::CollectionModFile& mod, const std::string& game_domain) {
  // No top-level modId on revision mods - it comes from nested file.modId,
  // which stays 0 when the file was removed server-side (file: null).
  if (mod.mod_id == 0)
    return std::nullopt;
  SourceNexus src;
  src.resolution = SourceResolution::Api;
  src.game_domain = game_domain;
  src.mod_id = mod.mod_id;
  src.file_id = mod.file_id;
  src.version = mod.version;
  src.file_name = mod.file_name;
  src.update_policy = mod.update_policy == "latest" ? UpdatePolicy::Latest
                                                   : UpdatePolicy::Exact;
  return src;
}

RevisionManifest revision_to_manifest(
    const nexus_v2::CollectionRevision& rev) {
  RevisionManifest out;
  Manifest& m = out.manifest;
  m.schema_version = "nexus/2";
  m.id = rev.slug.empty() ? "nexus-collection-" + std::to_string(rev.collection_id)
                          : "nexus-" + rev.slug;
  m.revision = rev.revision_number;
  m.info.name = rev.name;
  m.info.game_id = rev.game_domain;

  for (const auto& mod : rev.mods) {
    auto src = mod_file_to_source(mod, rev.game_domain);
    if (!src) {
      SkipDiagnostic d;
      d.mod_label = mod.file_name.empty()
                        ? "file_id " + std::to_string(mod.file_id)
                        : mod.file_name;
      d.reason = "file removed upstream (file: null), modId unknown";
      out.skipped.push_back(std::move(d));
      continue;
    }
    ModEntry entry;
    entry.id = "nexus-" + std::to_string(src->mod_id);
    entry.name = src->file_name.empty() ? entry.id : src->file_name;
    entry.phase = 0;
    entry.category =
        mod.optional ? ModCategory::Optional : ModCategory::Required;
    entry.source = *src;
    m.mods.push_back(std::move(entry));
  }

  for (const auto& res : rev.external_resources) {
    if (res.url.empty()) {
      SkipDiagnostic d;
      d.mod_label = res.name.empty() ? "external resource" : res.name;
      d.reason = "empty resource URL";
      out.skipped.push_back(std::move(d));
      continue;
    }
    ModEntry entry;
    entry.id = "external-" + std::to_string(m.mods.size());
    entry.name = res.name.empty() ? res.url : res.name;
    entry.phase = 0;
    entry.category =
        res.optional ? ModCategory::Optional : ModCategory::Required;
    SourceDirect direct;
    direct.resolution = SourceResolution::Browser;
    direct.url = res.url;
    direct.version = res.version;
    direct.file_name = res.name;
    entry.source = direct;
    m.mods.push_back(std::move(entry));
  }

  return out;
}

AccountStatus account_status() {
  const auto& auth = Source::Nexus::Auth::instance();
  AccountStatus status;
  status.authenticated = auth.has_api_key();
  status.can_auto_download = nexus_v2::is_premium_user();
  return status;
}

RouteOutcome route_download(const ModSource& source) {
  return Collection::route_download(source, account_status());
}

Adapter::Adapter(RevisionFetcher fetcher) : fetcher_(std::move(fetcher)) {}

Adapter::Adapter() : Adapter(default_fetcher()) {}

FetchOutcome Adapter::fetch(const std::string& source_id) {
  last_skipped_.clear();

  const CollectionRef ref = parse_source_id(source_id);
  if (ref.is_file) {
    try {
      FetchResult result;
      result.source_id = source_id;
      result.manifest = parse_file(ref.file_path);
      return result;
    } catch (const ParseError& e) {
      return FetchError{e.what(), 0};
    }
  }

  if (ref.slug.empty())
    return FetchError{"unrecognized Nexus collection id '" + source_id + "'",
                      0};

  nexus_v2::FetchResult fetched = fetcher_(ref.slug, ref.revision);
  if (!fetched.ok)
    return FetchError{fetched.error, 0};

  RevisionManifest converted = revision_to_manifest(fetched.revision);
  last_skipped_ = std::move(converted.skipped);
  FetchResult result;
  result.source_id = source_id;
  result.manifest = std::move(converted.manifest);
  return result;
}

bool Adapter::can_handle(const std::string& source_id) const {
  if (source_id.empty())
    return false;
  if (is_json_path(source_id))
    return true;
  if (is_nexus_collection_url(source_id))
    return !parse_url(source_id).slug.empty();
  return is_bare_slug(source_id);
}

}  // namespace engine::Collection::Nexus
