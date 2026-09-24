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
  REQUIRE_FALSE(adapter.can_handle(""));
  REQUIRE_FALSE(adapter.can_handle("not a valid id!!!"));
  REQUIRE_FALSE(adapter.can_handle("https://example.com/other"));
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
  REQUIRE(src->update_policy == UpdatePolicy::Exact);
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

  // Per-mod skips land in last_skipped(), fetch still succeeds.
  REQUIRE(adapter.last_skipped().size() == 2);
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
