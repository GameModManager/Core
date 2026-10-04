// Tests for engine::Collection::Nexus::Adapter - the Nexus-specific
// CollectionProvider (src/engine/collection/nexus/adapter.{h,cpp}).
//
// Covers every path:
//   - parse_source_id: bare slugs, slug@revision, collection URLs, .json paths
//   - mod_file_to_source: valid mods, null-file skips, update policies
//   - revision_to_manifest: mods, null-file + empty-URL skips, externals
//   - Adapter::fetch: GraphQL success/error, file path, unrecognized ids
//   - route_download glue: premium-vs-free via the generic router

#include "engine/collection/nexus/adapter.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <variant>

namespace fs = std::filesystem;
using namespace engine::Collection;
using namespace engine::Collection::Nexus;
using engine::nexus_v2::CollectionModFile;
using engine::nexus_v2::CollectionRevision;
using engine::nexus_v2::ExternalResource;
using NexusFetchResult      = engine::nexus_v2::FetchResult;
using CollectionFetchResult = engine::Collection::FetchResult;

namespace {

// A revision fetcher that always fails (for file-path tests where the
// fetcher must never be called).
Nexus::Adapter::RevisionFetcher dead_fetcher() {
  return [](const std::string &, long long) -> NexusFetchResult {
    NexusFetchResult r;
    r.ok    = false;
    r.error = "fetcher must not be called for file ids";
    return r;
  };
}

CollectionRevision make_revision() {
  CollectionRevision rev;
  rev.found           = true;
  rev.collection_id   = 42;
  rev.revision_number = 3;
  rev.slug            = "test-collection";
  rev.name            = "Test Collection";
  rev.game_domain     = "skyrimspecialedition";
  rev.revision_status = "published";

  CollectionModFile good;
  good.mod_id        = 17464;
  good.file_id       = 19080;
  good.game_id       = 1704;
  good.version       = "0.4.20";
  good.file_name     = "RaceMenu.7z";
  good.update_policy = "exact";
  good.optional      = false;
  rev.mods.push_back(good);

  // file: null upstream - mod_id stays 0, must be skipped with diagnostic.
  CollectionModFile removed;
  removed.file_id       = 99;
  removed.game_id       = 1704;
  removed.version       = "1.0";
  removed.file_name     = "";
  removed.update_policy = "latest";
  removed.optional      = true;
  rev.mods.push_back(removed);

  ExternalResource ext;
  ext.name     = "SKSE";
  ext.url      = "https://example.com/skse.7z";
  ext.version  = "2.2";
  ext.optional = false;
  rev.external_resources.push_back(ext);

  ExternalResource empty;
  empty.name = "Broken";
  empty.url  = "";
  rev.external_resources.push_back(empty);

  return rev;
}

// The collection.json the revision's .zip carries: the per-file hashes, the
// FOMOD selections, the load order and the tag that the metadata query has no
// field for at all.
const char *kArchiveJson = R"({
  "info": { "installInstructions": "Clean save first.", "gameVersions": ["1.7.2"] },
  "mods": [
    { "name": "RaceMenu", "version": "0.4.20",
      "source": { "type": "nexus", "modId": 17464, "fileId": 19080,
                  "md5": "0a6e3e603ef3bca799436f69510c79b7",
                  "fileSize": 4242, "tag": "JqF6xzzWA" },
      "hashes": [ { "path": "meshes\\actors\\foo.esm",
                    "md5": "add39f916aa4f469b51881fe6b50a9c6" } ],
      "instructions": "Do not also install SKSE." }
  ],
  "loadOrder": ["RaceMenu.esm"],
  "modRules": [
    { "type": "requires",
      "source": { "tag": "JqF6xzzWA" },
      "reference": { "tag": "x_A_Q2gQ3e" } }
  ]
})";

}  // namespace

// ---------------------------------------------------------------------------
// parse_source_id
// ---------------------------------------------------------------------------

TEST_CASE("parse_source_id bare slug", "[collection][nexus][adapter]") {
  const auto ref = parse_source_id("my-collection");
  REQUIRE(ref.slug == "my-collection");
  REQUIRE(ref.revision == 0);
  REQUIRE_FALSE(ref.is_file);
}

TEST_CASE("parse_source_id slug at revision", "[collection][nexus][adapter]") {
  const auto ref = parse_source_id("my-collection@7");
  REQUIRE(ref.slug == "my-collection");
  REQUIRE(ref.revision == 7);
  REQUIRE_FALSE(ref.is_file);
}

TEST_CASE("parse_source_id bad revision yields 0", "[collection][nexus][adapter]") {
  const auto ref = parse_source_id("my-collection@abc");
  REQUIRE(ref.slug == "my-collection");
  REQUIRE(ref.revision == 0);
}

TEST_CASE("parse_source_id collection URL", "[collection][nexus][adapter]") {
  const auto ref = parse_source_id(
      "https://www.nexusmods.com/skyrimspecialedition/collections/abcd");
  REQUIRE(ref.slug == "abcd");
  REQUIRE(ref.revision == 0);
  REQUIRE_FALSE(ref.is_file);
}

TEST_CASE("parse_source_id collection URL with revision path",
          "[collection][nexus][adapter]") {
  const auto ref =
      parse_source_id("https://www.nexusmods.com/skyrimspecialedition/collections/abcd/"
                      "revisions/5");
  REQUIRE(ref.slug == "abcd");
  REQUIRE(ref.revision == 5);
}

TEST_CASE("parse_source_id collection URL with revision query",
          "[collection][nexus][adapter]") {
  const auto ref =
      parse_source_id("https://www.nexusmods.com/skyrimspecialedition/collections/abcd"
                      "?revision=9");
  REQUIRE(ref.slug == "abcd");
  REQUIRE(ref.revision == 9);
}

TEST_CASE("parse_source_id json path", "[collection][nexus][adapter]") {
  const auto ref = parse_source_id("/tmp/export/collection.json");
  REQUIRE(ref.is_file);
  REQUIRE(ref.file_path == "/tmp/export/collection.json");
}

// nxm://<game>/collections/<id>/revisions/<n> - the shape the Nexus site and
// the official app emit. Before this was parsed, the whole URL was handed to
// the collection API as a slug.
TEST_CASE("parse_source_id nxm collection link", "[collection][nexus][adapter]") {
  const auto ref = parse_source_id(
      "nxm://skyrimspecialedition/collections/hygge-for-lore-and-4096/revisions/7");
  REQUIRE_FALSE(ref.is_file);
  REQUIRE(ref.slug == "hygge-for-lore-and-4096");
  REQUIRE(ref.revision == 7);
}

// A link that pins no revision means "latest", and 0 is what the fetcher
// turns into the nullable revision argument the query documents as "the latest
// published revision".
TEST_CASE("parse_source_id nxm collection link without a revision",
          "[collection][nexus][adapter]") {
  const auto ref =
      parse_source_id("nxm://skyrimspecialedition/collections/hygge-for-lore");
  REQUIRE(ref.slug == "hygge-for-lore");
  REQUIRE(ref.revision == 0);
}

TEST_CASE("parse_source_id nxm collection link past the nexus authority",
          "[collection][nexus][adapter]") {
  const auto ref =
      parse_source_id("nxm://nexus/cyberpunk2077/collections/neon/revisions/2");
  REQUIRE(ref.slug == "neon");
  REQUIRE(ref.revision == 2);
}

TEST_CASE("parse_source_id rejects nxm links that are not collections",
          "[collection][nexus][adapter]") {
  // A mod link has no collection for the collection API to look up. An empty
  // slug is what stops it being sent as one.
  REQUIRE(parse_source_id("nxm://skyrimspecialedition/mods/184625/files/781833")
              .slug.empty());
  // Malformed collection links: no id at all, and a revision that is not a
  // number (which must not be silently taken for "latest").
  REQUIRE(parse_source_id("nxm://skyrimspecialedition/collections/").slug.empty());
  REQUIRE(parse_source_id("nxm://skyrimspecialedition/collections/abcd/revisions/abc")
              .slug.empty());
  REQUIRE(parse_source_id("nxm://skyrimspecialedition/collections/abcd/revisions/-1")
              .slug.empty());
}

TEST_CASE("Adapter fetch sends an nxm collection link's id, not the whole URL",
          "[collection][nexus][adapter]") {
  std::string seen_slug;
  long long seen_revision = -1;
  Nexus::Adapter adapter(
      [&](const std::string &slug, long long revision) -> NexusFetchResult {
        seen_slug     = slug;
        seen_revision = revision;
        NexusFetchResult r;
        r.ok       = true;
        r.revision = make_revision();
        return r;
      });

  const std::string url =
      "nxm://skyrimspecialedition/collections/hygge-for-lore/revisions/7";
  const auto outcome = adapter.fetch(url);
  REQUIRE(std::holds_alternative<CollectionFetchResult>(outcome));
  // The regression this replaces: the entire nxm:// URL used to arrive here as
  // the slug, so GraphQL was asked for a collection named after a URL.
  REQUIRE(seen_slug == "hygge-for-lore");
  REQUIRE(seen_slug.find("nxm://") == std::string::npos);
  REQUIRE(seen_revision == 7);
}

TEST_CASE("Adapter fetch nxm collection link without a revision asks for latest",
          "[collection][nexus][adapter]") {
  std::string seen_slug;
  long long seen_revision = -1;
  Nexus::Adapter adapter(
      [&](const std::string &slug, long long revision) -> NexusFetchResult {
        seen_slug     = slug;
        seen_revision = revision;
        NexusFetchResult r;
        r.ok       = true;
        r.revision = make_revision();
        return r;
      });

  const auto outcome =
      adapter.fetch("nxm://skyrimspecialedition/collections/hygge-for-lore");
  REQUIRE(std::holds_alternative<CollectionFetchResult>(outcome));
  REQUIRE(seen_slug == "hygge-for-lore");
  // 0 is the "omit the revision variable, let Nexus pick latest published"
  // signal - see build_collection_revision_variables.
  REQUIRE(seen_revision == 0);
}

TEST_CASE("Adapter fetch refuses a malformed nxm link with a useful error",
          "[collection][nexus][adapter]") {
  // The fetcher must never run: a bad link has no id to ask about.
  bool fetched = false;
  Nexus::Adapter adapter([&](const std::string &, long long) -> NexusFetchResult {
    fetched = true;
    NexusFetchResult r;
    r.ok    = false;
    r.error = "must not be called";
    return r;
  });

  // Each section pins the DISTINCT diagnosis. One generic message would report a
  // malformed collection link as a mod link - which it is not, and which hides
  // the part that is actually broken.
  SECTION("mod link") {
    const auto outcome =
        adapter.fetch("nxm://skyrimspecialedition/mods/184625/files/781833");
    REQUIRE(std::holds_alternative<FetchError>(outcome));
    const auto &msg = std::get<FetchError>(outcome).message;
    REQUIRE(msg.find("link to a mod") != std::string::npos);
    REQUIRE(msg.find("nxm://<game>/collections/<id>") != std::string::npos);
    REQUIRE_FALSE(fetched);
  }

  SECTION("collection link with no id") {
    const auto outcome = adapter.fetch("nxm://skyrimspecialedition/collections/");
    REQUIRE(std::holds_alternative<FetchError>(outcome));
    const auto &msg = std::get<FetchError>(outcome).message;
    REQUIRE(msg.find("no collection id") != std::string::npos);
    REQUIRE(msg.find("nxm://<game>/collections/<id>") != std::string::npos);
    REQUIRE_FALSE(fetched);
  }

  SECTION("collection link with an unreadable revision") {
    const auto outcome =
        adapter.fetch("nxm://skyrimspecialedition/collections/abcd/revisions/abc");
    REQUIRE(std::holds_alternative<FetchError>(outcome));
    const auto &msg = std::get<FetchError>(outcome).message;
    REQUIRE(msg.find("not a positive number") != std::string::npos);
    // It IS a collection link, so it must not be told it is a mod link, and
    // the revision must not be silently resolved to "latest".
    REQUIRE(msg.find("link to a mod") == std::string::npos);
    REQUIRE_FALSE(fetched);
  }
}

TEST_CASE("parse_source_id garbage yields empty slug", "[collection][nexus][adapter]") {
  const auto ref = parse_source_id("not a valid id!!!");
  REQUIRE(ref.slug == "not a valid id!!!");
  REQUIRE_FALSE(ref.is_file);
}

// ---------------------------------------------------------------------------
// can_handle
// ---------------------------------------------------------------------------

TEST_CASE("Adapter can_handle", "[collection][nexus][adapter]") {
  Nexus::Adapter adapter(dead_fetcher());
  REQUIRE(adapter.can_handle("/tmp/x/collection.json"));
  REQUIRE(adapter.can_handle(
      "https://www.nexusmods.com/skyrimspecialedition/collections/abcd"));
  REQUIRE(adapter.can_handle("my-collection"));
  REQUIRE(adapter.can_handle("my-collection@3"));
  REQUIRE(
      adapter.can_handle("nxm://skyrimspecialedition/collections/abcd/revisions/3"));
  REQUIRE(adapter.can_handle("nxm://skyrimspecialedition/collections/abcd"));
  REQUIRE_FALSE(adapter.can_handle(""));
  REQUIRE_FALSE(adapter.can_handle("not a valid id!!!"));
  REQUIRE_FALSE(adapter.can_handle("https://example.com/other"));
  // An nxm:// mod link is a download, not a collection.
  REQUIRE_FALSE(
      adapter.can_handle("nxm://skyrimspecialedition/mods/184625/files/781833"));
  REQUIRE_FALSE(adapter.can_handle("nxm://skyrimspecialedition/collections/"));
  REQUIRE_FALSE(
      adapter.can_handle("nxm://skyrimspecialedition/collections/abcd/revisions/abc"));
}

TEST_CASE("Adapter identity", "[collection][nexus][adapter]") {
  Nexus::Adapter adapter(dead_fetcher());
  REQUIRE(adapter.source_type() == "nexus_collection");
  REQUIRE(adapter.display_name() == "Nexus Collections");
}

// ---------------------------------------------------------------------------
// mod_file_to_source
// ---------------------------------------------------------------------------

TEST_CASE("mod_file_to_source maps fields", "[collection][nexus][adapter]") {
  CollectionModFile mod;
  mod.mod_id        = 17464;
  mod.file_id       = 19080;
  mod.version       = "0.4.20";
  mod.file_name     = "RaceMenu.7z";
  mod.update_policy = "exact";

  const auto src = mod_file_to_source(mod, "skyrimspecialedition");
  REQUIRE(src.has_value());
  REQUIRE(src->game_domain == "skyrimspecialedition");
  REQUIRE(src->mod_id == 17464);
  REQUIRE(src->file_id == 19080);
  REQUIRE(src->version == "0.4.20");
  REQUIRE(src->file_name == "RaceMenu.7z");
  REQUIRE(src->resolution == SourceResolution::Api);
  // Nexus says "exact"; we keep the pin and allow the fallback, because the
  // md5 it publishes cannot back a 64-hex sha256 pin.
  REQUIRE(src->update_policy == UpdatePolicy::Prefer);
}

TEST_CASE("mod_file_to_source keeps the three states distinct",
          "[collection][nexus][adapter]") {
  CollectionModFile mod;
  mod.mod_id  = 1;
  mod.file_id = 2;

  mod.update_policy = "prefer";
  REQUIRE(mod_file_to_source(mod, "fallout4")->update_policy == UpdatePolicy::Prefer);

  mod.update_policy = "latest";
  REQUIRE(mod_file_to_source(mod, "fallout4")->update_policy == UpdatePolicy::Latest);

  mod.update_policy = "exact";
  REQUIRE(mod_file_to_source(mod, "fallout4")->update_policy == UpdatePolicy::Prefer);

  // No value ever reads back as the digest-backed pin we cannot produce.
  mod.update_policy = "";
  REQUIRE(mod_file_to_source(mod, "fallout4")->update_policy != UpdatePolicy::Exact);
}

TEST_CASE("mod_file_to_source latest policy", "[collection][nexus][adapter]") {
  CollectionModFile mod;
  mod.mod_id        = 1;
  mod.file_id       = 2;
  mod.update_policy = "latest";

  const auto src = mod_file_to_source(mod, "fallout4");
  REQUIRE(src.has_value());
  REQUIRE(src->update_policy == UpdatePolicy::Latest);
}

TEST_CASE("mod_file_to_source null file skipped", "[collection][nexus][adapter]") {
  // file: null upstream leaves mod_id at 0 - nothing to resolve against.
  CollectionModFile mod;
  mod.file_id       = 99;
  mod.update_policy = "latest";
  REQUIRE_FALSE(mod_file_to_source(mod, "skyrimspecialedition").has_value());
}

// ---------------------------------------------------------------------------
// revision_to_manifest
// ---------------------------------------------------------------------------

TEST_CASE("revision_to_manifest converts mods and externals",
          "[collection][nexus][adapter]") {
  const auto out = revision_to_manifest(make_revision());

  REQUIRE(out.manifest.id == "nexus-test-collection");
  REQUIRE(out.manifest.revision == 3);
  REQUIRE(out.manifest.info.name == "Test Collection");
  REQUIRE(out.manifest.info.game_id == "skyrimspecialedition");

  // 1 Nexus mod + 1 external; removed file + empty URL skipped.
  REQUIRE(out.manifest.mods.size() == 2);

  const auto *nexus_src = std::get_if<SourceNexus>(&out.manifest.mods[0].source);
  REQUIRE(nexus_src != nullptr);
  REQUIRE(nexus_src->mod_id == 17464);
  REQUIRE(nexus_src->file_id == 19080);
  REQUIRE(nexus_src->game_domain == "skyrimspecialedition");
  REQUIRE(out.manifest.mods[0].category == ModCategory::Required);

  const auto *direct_src = std::get_if<SourceDirect>(&out.manifest.mods[1].source);
  REQUIRE(direct_src != nullptr);
  REQUIRE(direct_src->url == "https://example.com/skse.7z");
  REQUIRE(direct_src->resolution == SourceResolution::Browser);

  REQUIRE(out.skipped.size() == 2);
}

TEST_CASE("revision_to_manifest falls back to collection id",
          "[collection][nexus][adapter]") {
  CollectionRevision rev;
  rev.collection_id = 77;
  rev.game_domain   = "fallout4";
  const auto out    = revision_to_manifest(rev);
  REQUIRE(out.manifest.id == "nexus-collection-77");
  REQUIRE(out.manifest.mods.empty());
  REQUIRE(out.skipped.empty());
}

TEST_CASE("revision_to_manifest carries the revision status and every skip",
          "[collection][nexus][adapter]") {
  auto rev            = make_revision();
  rev.revision_status = "retracted";

  const auto out = revision_to_manifest(rev);

  // A retracted revision still has a mod list; the status is what tells the
  // importer that the list is no longer what the author recommends.
  REQUIRE(out.revision_status == "retracted");
  REQUIRE(out.manifest.mods.size() == 2);

  // Both skips, each with a reason the user can act on - not just a count.
  REQUIRE(out.skipped.size() == 2);
  bool saw_removed_file = false;
  for (const auto &s : out.skipped) {
    REQUIRE_FALSE(s.mod_label.empty());
    REQUIRE_FALSE(s.reason.empty());
    if (s.reason.find("removed upstream") != std::string::npos) {
      saw_removed_file = true;
      REQUIRE(s.mod_label == "file_id 99");
    }
  }
  REQUIRE(saw_removed_file);
}

// ---------------------------------------------------------------------------
// Adapter::fetch - GraphQL path (injected fetcher, no network)
// ---------------------------------------------------------------------------

TEST_CASE("Adapter fetch revision success", "[collection][nexus][adapter]") {
  std::string seen_slug;
  long long seen_revision = -1;
  Nexus::Adapter adapter(
      [&](const std::string &slug, long long revision) -> NexusFetchResult {
        seen_slug     = slug;
        seen_revision = revision;
        NexusFetchResult r;
        r.ok       = true;
        r.revision = make_revision();
        return r;
      });

  const auto outcome = adapter.fetch("test-collection@3");
  REQUIRE(std::holds_alternative<CollectionFetchResult>(outcome));
  const auto &result = std::get<CollectionFetchResult>(outcome);
  REQUIRE(result.source_id == "test-collection@3");
  REQUIRE(result.manifest.mods.size() == 2);
  REQUIRE(seen_slug == "test-collection");
  REQUIRE(seen_revision == 3);

  // Per-mod skips and the revision status land on the adapter, so the import
  // dialog can report them instead of importing a quietly shorter list.
  REQUIRE(adapter.last_skipped().size() == 2);
  REQUIRE(adapter.last_revision_status() == "published");
}

// ---------------------------------------------------------------------------
// Adapter::fetch - the revision's own archive (collection.json inside the zip)
// ---------------------------------------------------------------------------

TEST_CASE("Adapter fetch merges collection.json over the metadata",
          "[collection][nexus][adapter]") {
  std::string seen_link;
  auto rev          = make_revision();
  rev.download_link = "https://cdn.nexusmods.com/collection/42/3.zip";
  Nexus::Adapter adapter(
      [rev](const std::string &, long long) -> NexusFetchResult {
        NexusFetchResult r;
        r.ok       = true;
        r.revision = rev;
        return r;
      },
      [&](const std::string &link, std::string &json, std::string &) {
        seen_link = link;
        json      = kArchiveJson;
        return true;
      });

  const auto outcome = adapter.fetch("test-collection@3");
  REQUIRE(std::holds_alternative<CollectionFetchResult>(outcome));
  const auto &result = std::get<CollectionFetchResult>(outcome);
  const auto &m      = result.manifest;

  // The archive is fetched at the link the metadata reported.
  REQUIRE(seen_link == "https://cdn.nexusmods.com/collection/42/3.zip");

  // Metadata's own contribution survives: the identity, the revision status,
  // and the external-resource entry the archive never mentions. The two
  // unresolvable entries are still skipped with their diagnostics.
  REQUIRE(m.info.name == "Test Collection");
  REQUIRE(m.info.game_id == "skyrimspecialedition");
  REQUIRE(adapter.last_revision_status() == "published");
  REQUIRE(adapter.last_skipped().size() == 2);
  REQUIRE(m.mods.size() == 2);

  // The archive's contribution, none of which GraphQL could have supplied.
  REQUIRE(m.info.install_instructions == "Clean save first.");
  REQUIRE(m.info.game_versions.size() == 1);
  REQUIRE(m.load_order.plugin_hint.size() == 1);
  REQUIRE(m.load_order.plugin_hint[0] == "RaceMenu.esm");

  const auto &racemenu = m.mods[0];
  REQUIRE(racemenu.id == "nexus-17464");
  REQUIRE(racemenu.hashes.size() == 1);
  REQUIRE(racemenu.hashes[0].md5 == "add39f916aa4f469b51881fe6b50a9c6");
  REQUIRE(racemenu.instructions == "Do not also install SKSE.");
  const auto &nx = std::get<SourceNexus>(racemenu.source);
  REQUIRE(nx.tag == "JqF6xzzWA");
  REQUIRE(nx.md5 == "0a6e3e603ef3bca799436f69510c79b7");
  REQUIRE(nx.file_size == 4242);
  REQUIRE(nx.sha256.empty());

  // A rule end the archive itself cannot bind is reported, not kept dangling
  // and not silently dropped.
  REQUIRE(m.rules.empty());
  REQUIRE(adapter.last_unresolved().size() == 1);
  REQUIRE(adapter.last_unresolved()[0].what == "modRules[0].reference");
  REQUIRE(adapter.last_unresolved()[0].reason.find("tag=x_A_Q2gQ3e") !=
          std::string::npos);
}

TEST_CASE("Adapter fetch reports an unreachable archive and still imports",
          "[collection][nexus][adapter]") {
  auto rev          = make_revision();
  rev.download_link = "https://cdn.nexusmods.com/collection/42/3.zip";
  Nexus::Adapter adapter(
      [rev](const std::string &, long long) -> NexusFetchResult {
        NexusFetchResult r;
        r.ok       = true;
        r.revision = rev;
        return r;
      },
      [](const std::string &, std::string &, std::string &error) {
        error = "could not download the collection archive (HTTP 401)";
        return false;
      });

  const auto outcome = adapter.fetch("test-collection@3");

  // Not an error: the import proceeds on metadata alone.
  REQUIRE(std::holds_alternative<CollectionFetchResult>(outcome));
  const auto &m = std::get<CollectionFetchResult>(outcome).manifest;
  REQUIRE(m.info.name == "Test Collection");
  REQUIRE(m.mods.size() == 2);

  // And it says exactly what it could not read, naming the archive and what it
  // would have carried.
  REQUIRE(adapter.last_unresolved().size() == 1);
  const auto &gap = adapter.last_unresolved()[0];
  REQUIRE(gap.what == "collection.json");
  REQUIRE(gap.reason.find("HTTP 401") != std::string::npos);
  REQUIRE(gap.reason.find("load order") != std::string::npos);
  REQUIRE(gap.reason.find("per-file hashes") != std::string::npos);
  REQUIRE(gap.reason.find("mod rules") != std::string::npos);
}

TEST_CASE("Adapter fetch reports a missing download link",
          "[collection][nexus][adapter]") {
  auto rev = make_revision();  // no download_link
  Nexus::Adapter adapter(
      [rev](const std::string &, long long) -> NexusFetchResult {
        NexusFetchResult r;
        r.ok       = true;
        r.revision = rev;
        return r;
      },
      [](const std::string &, std::string &, std::string &) {
        return false;  // must never be asked
      });

  REQUIRE(
      std::holds_alternative<CollectionFetchResult>(adapter.fetch("test-collection")));
  REQUIRE(adapter.last_unresolved().size() == 1);
  REQUIRE(adapter.last_unresolved()[0].what == "collection.json");
  REQUIRE(adapter.last_unresolved()[0].reason.find("no download link") !=
          std::string::npos);
}

TEST_CASE("Adapter fetch survives an unreadable archive",
          "[collection][nexus][adapter]") {
  auto rev          = make_revision();
  rev.download_link = "https://cdn.nexusmods.com/collection/42/3.zip";
  Nexus::Adapter adapter(
      [rev](const std::string &, long long) -> NexusFetchResult {
        NexusFetchResult r;
        r.ok       = true;
        r.revision = rev;
        return r;
      },
      [](const std::string &, std::string &json, std::string &) {
        json = "{ this is not json";
        return true;
      });

  const auto outcome = adapter.fetch("test-collection");
  REQUIRE(std::holds_alternative<CollectionFetchResult>(outcome));
  const auto &m = std::get<CollectionFetchResult>(outcome).manifest;
  REQUIRE(m.mods.size() == 2);
  REQUIRE(adapter.last_unresolved().size() == 1);
  REQUIRE(adapter.last_unresolved()[0].reason.find("not readable") !=
          std::string::npos);
}

TEST_CASE("Adapter fetch revision error", "[collection][nexus][adapter]") {
  Nexus::Adapter adapter([](const std::string &, long long) -> NexusFetchResult {
    NexusFetchResult r;
    r.ok    = false;
    r.error = "collection not found";
    return r;
  });

  const auto outcome = adapter.fetch("nope");
  REQUIRE(std::holds_alternative<FetchError>(outcome));
  REQUIRE(std::get<FetchError>(outcome).message == "collection not found");
  REQUIRE(adapter.last_skipped().empty());
}

TEST_CASE("Adapter fetch unrecognized id", "[collection][nexus][adapter]") {
  Nexus::Adapter adapter(dead_fetcher());
  const auto outcome = adapter.fetch("not a valid id!!!");
  REQUIRE(std::holds_alternative<FetchError>(outcome));
  REQUIRE_FALSE(std::get<FetchError>(outcome).message.empty());
}

// ---------------------------------------------------------------------------
// Adapter::fetch - collection.json file path
// ---------------------------------------------------------------------------

TEST_CASE("Adapter fetch collection.json file", "[collection][nexus][adapter]") {
  const fs::path dir =
      fs::temp_directory_path() / ("gmm_nexus_adapter_" + std::to_string(getpid()));
  fs::create_directories(dir);
  const fs::path file = dir / "collection.json";
  {
    std::ofstream out(file);
    out << R"({
      "info": {"name": "File Pack", "domainName": "skyrimspecialedition"},
      "mods": []
    })";
  }

  Nexus::Adapter adapter(dead_fetcher());
  const auto outcome = adapter.fetch(file.string());
  REQUIRE(std::holds_alternative<CollectionFetchResult>(outcome));
  const auto &result = std::get<CollectionFetchResult>(outcome);
  REQUIRE(result.manifest.info.name == "File Pack");
  REQUIRE(adapter.last_skipped().empty());

  fs::remove_all(dir);
}

TEST_CASE("Adapter fetch missing file is FetchError", "[collection][nexus][adapter]") {
  Nexus::Adapter adapter(dead_fetcher());
  const auto outcome = adapter.fetch("/tmp/gmm-does-not-exist/collection.json");
  REQUIRE(std::holds_alternative<FetchError>(outcome));
}

// ---------------------------------------------------------------------------
// route_download glue (premium vs free via the generic router)
// ---------------------------------------------------------------------------

TEST_CASE("Adapter route_download browser entries stay browser",
          "[collection][nexus][adapter]") {
  // Redirect Auth disk state before the singleton is first touched so the
  // test never depends on the developer's real Nexus credentials.
  const fs::path config = fs::temp_directory_path() /
                          ("gmm_nexus_adapter_auth_" + std::to_string(getpid()));
  fs::create_directories(config);
  setenv("XDG_CONFIG_HOME", config.c_str(), 1);

  SourceDirect direct;
  direct.resolution  = SourceResolution::Browser;
  direct.url         = "https://example.com/mod.7z";
  const auto outcome = Nexus::route_download(direct);
  REQUIRE(outcome.path == DownloadPath::Browser);

  fs::remove_all(config);
}
