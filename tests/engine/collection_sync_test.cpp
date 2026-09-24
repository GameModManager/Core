// Tests for source-agnostic collection sync/tracking (Workspace-5wmu).
//
// Covers:
//   - engine::Mod tracking-field defaults
//   - tag / untag / belongs_to membership helpers
//   - compare_revision (Older / Same / Newer)
//   - entry_version across all ModSource variants
//   - diff_revision: added / removed / updated / unchanged, with
//     untracked and foreign-collection mods ignored
//   - Resolver stamps collection membership on resolve()
//   - ModMeta collection key roundtrip + clear_collection

#include "engine/collection/manifest.h"
#include "engine/collection/resolver.h"
#include "engine/collection/sync.h"
#include "engine/mod/meta/mod_meta.h"
#include "engine/mod/model/mod.h"

#include <catch2/catch_test_macros.hpp>

using namespace engine::Collection;

namespace {

// Build a manifest entry with a Nexus source at the given version.
ModEntry nexus_entry(const std::string &id, const std::string &version) {
  ModEntry e;
  e.id   = id;
  e.name = id;
  SourceNexus s;
  s.game_domain = "skyrimspecialedition";
  s.mod_id      = 1234;
  s.file_id     = 1;
  s.version     = version;
  s.file_name   = id + ".zip";
  e.source      = s;
  return e;
}

Manifest manifest_with(std::vector<ModEntry> mods, const std::string &id = "pack-uuid",
                       int64_t revision = 2) {
  Manifest m;
  m.schema_version = "1.0.0";
  m.id             = id;
  m.revision       = revision;
  m.mods           = std::move(mods);
  return m;
}

::engine::Mod installed_mod(const std::string &id, const std::string &version,
                            const std::string &collection_id = "pack-uuid",
                            int64_t revision                 = 1) {
  ::engine::Mod mod;
  mod.id      = id;
  mod.version = version;
  tag(mod, collection_id, revision);
  return mod;
}

}  // namespace

// ---------------------------------------------------------------------------
// Mod tracking-field defaults
// ---------------------------------------------------------------------------

TEST_CASE("Mod is untracked by default", "[collection][sync]") {
  ::engine::Mod mod;
  CHECK(mod.collection_id.empty());
  CHECK(mod.collection_revision == 0);
  CHECK_FALSE(mod.in_collection);
}

// ---------------------------------------------------------------------------
// tag / untag / belongs_to
// ---------------------------------------------------------------------------

TEST_CASE("tag marks membership", "[collection][sync]") {
  ::engine::Mod mod;
  mod.id = "some-mod";
  tag(mod, "pack-uuid", 3);
  CHECK(mod.collection_id == "pack-uuid");
  CHECK(mod.collection_revision == 3);
  CHECK(mod.in_collection);
  CHECK(belongs_to(mod, "pack-uuid"));
  CHECK_FALSE(belongs_to(mod, "other-pack"));
}

TEST_CASE("untag clears membership", "[collection][sync]") {
  ::engine::Mod mod = installed_mod("m", "1.0");
  untag(mod);
  CHECK(mod.collection_id.empty());
  CHECK(mod.collection_revision == 0);
  CHECK_FALSE(mod.in_collection);
  CHECK_FALSE(belongs_to(mod, "pack-uuid"));
}

TEST_CASE("belongs_to requires the flag, not just the id", "[collection][sync]") {
  ::engine::Mod mod;
  mod.id            = "m";
  mod.collection_id = "pack-uuid";  // stale id without the flag
  CHECK_FALSE(belongs_to(mod, "pack-uuid"));
}

// ---------------------------------------------------------------------------
// compare_revision
// ---------------------------------------------------------------------------

TEST_CASE("compare_revision orders revisions", "[collection][sync]") {
  CHECK(compare_revision(1, 2) == RevisionRelation::Newer);
  CHECK(compare_revision(2, 2) == RevisionRelation::Same);
  CHECK(compare_revision(3, 2) == RevisionRelation::Older);
}

// ---------------------------------------------------------------------------
// entry_version across source variants
// ---------------------------------------------------------------------------

TEST_CASE("entry_version reads every source variant", "[collection][sync]") {
  ModEntry nexus = nexus_entry("n", "1.2");
  CHECK(entry_version(nexus) == "1.2");

  ModEntry ll;
  ll.id = "ll";
  SourceLoversLab sll;
  sll.version = "3.0";
  ll.source   = sll;
  CHECK(entry_version(ll) == "3.0");

  ModEntry mp;
  mp.id = "mp";
  SourceModPub smp;
  smp.version = "0.9";
  mp.source   = smp;
  CHECK(entry_version(mp) == "0.9");

  // Steam Workshop versions are unknown ahead of time (empty).
  ModEntry sw;
  sw.id = "sw";
  SourceSteamWorkshop ssw;
  ssw.workshop_item_id = 42;
  sw.source            = ssw;
  CHECK(entry_version(sw).empty());

  ModEntry direct;
  direct.id = "d";
  SourceDirect sd;
  sd.url        = "https://example.com/mod.zip";
  sd.version    = "7";
  direct.source = sd;
  CHECK(entry_version(direct) == "7");
}

// ---------------------------------------------------------------------------
// diff_revision
// ---------------------------------------------------------------------------

TEST_CASE("diff on fresh install reports everything added", "[collection][sync]") {
  Manifest incoming = manifest_with({nexus_entry("a", "1.0"), nexus_entry("b", "2.0")});
  CollectionDiff diff = diff_revision({}, incoming);
  CHECK(diff.added == std::vector<std::string>{"a", "b"});
  CHECK(diff.removed.empty());
  CHECK(diff.updated.empty());
  CHECK(diff.unchanged.empty());
}

TEST_CASE("diff with no changes reports unchanged", "[collection][sync]") {
  Manifest incoming = manifest_with({nexus_entry("a", "1.0"), nexus_entry("b", "2.0")});
  std::vector<::engine::Mod> installed = {
      installed_mod("a", "1.0"),
      installed_mod("b", "2.0"),
  };
  CollectionDiff diff = diff_revision(installed, incoming);
  CHECK(diff.added.empty());
  CHECK(diff.removed.empty());
  CHECK(diff.updated.empty());
  CHECK(diff.unchanged == std::vector<std::string>{"a", "b"});
}

TEST_CASE("diff detects updated versions", "[collection][sync]") {
  Manifest incoming                    = manifest_with({nexus_entry("a", "1.1")});
  std::vector<::engine::Mod> installed = {installed_mod("a", "1.0")};
  CollectionDiff diff                  = diff_revision(installed, incoming);
  CHECK(diff.updated == std::vector<std::string>{"a"});
  CHECK(diff.added.empty());
  CHECK(diff.removed.empty());
  CHECK(diff.unchanged.empty());
}

TEST_CASE("diff detects removed mods", "[collection][sync]") {
  Manifest incoming                    = manifest_with({nexus_entry("a", "1.0")});
  std::vector<::engine::Mod> installed = {
      installed_mod("a", "1.0"),
      installed_mod("gone", "1.0"),
  };
  CollectionDiff diff = diff_revision(installed, incoming);
  CHECK(diff.removed == std::vector<std::string>{"gone"});
  CHECK(diff.unchanged == std::vector<std::string>{"a"});
  CHECK(diff.added.empty());
  CHECK(diff.updated.empty());
}

TEST_CASE("diff ignores untracked and foreign-collection mods", "[collection][sync]") {
  Manifest incoming = manifest_with({nexus_entry("a", "1.0")});

  ::engine::Mod standalone;
  standalone.id      = "standalone";
  standalone.version = "9.9";  // same id space, but untracked

  ::engine::Mod foreign = installed_mod("foreign-mod", "1.0", "other-pack", 5);

  std::vector<::engine::Mod> installed = {
      installed_mod("a", "1.0"),
      standalone,
      foreign,
  };
  CollectionDiff diff = diff_revision(installed, incoming);
  CHECK(diff.unchanged == std::vector<std::string>{"a"});
  CHECK(diff.added.empty());
  CHECK(diff.removed.empty());
  CHECK(diff.updated.empty());
}

TEST_CASE("diff update scenario: add + update + remove + keep", "[collection][sync]") {
  Manifest incoming                    = manifest_with({
      nexus_entry("keep", "1.0"),
      nexus_entry("bump", "2.0"),
      nexus_entry("fresh", "1.0"),
  });
  std::vector<::engine::Mod> installed = {
      installed_mod("keep", "1.0"),
      installed_mod("bump", "1.0"),
      installed_mod("dropped", "1.0"),
  };
  CollectionDiff diff = diff_revision(installed, incoming);
  CHECK(diff.added == std::vector<std::string>{"fresh"});
  CHECK(diff.removed == std::vector<std::string>{"dropped"});
  CHECK(diff.updated == std::vector<std::string>{"bump"});
  CHECK(diff.unchanged == std::vector<std::string>{"keep"});
}

// ---------------------------------------------------------------------------
// Resolver stamps membership
// ---------------------------------------------------------------------------

TEST_CASE("Resolver stamps collection tracking on resolve", "[collection][sync]") {
  Manifest incoming = manifest_with({nexus_entry("a", "1.0")}, "pack-uuid", 7);
  Resolver resolver;
  ResolvedCollection resolved = resolver.resolve(incoming);
  REQUIRE(resolved.mods.size() == 1);
  CHECK(resolved.mods[0].mod.collection_id == "pack-uuid");
  CHECK(resolved.mods[0].mod.collection_revision == 7);
  CHECK(resolved.mods[0].mod.in_collection);
  CHECK(belongs_to(resolved.mods[0].mod, "pack-uuid"));
}

// ---------------------------------------------------------------------------
// ModMeta persistence
// ---------------------------------------------------------------------------

TEST_CASE("ModMeta collection keys default to untracked", "[collection][sync]") {
  engine::ModMeta meta;
  CHECK(meta.collection_id().empty());
  CHECK(meta.collection_revision() == 0);
  CHECK_FALSE(meta.in_collection());
}

TEST_CASE("ModMeta collection roundtrip through serialize/parse",
          "[collection][sync]") {
  engine::ModMeta meta;
  meta.set_collection_id("pack-uuid");
  meta.set_collection_revision(4);
  meta.set_in_collection(true);

  engine::ModMeta reloaded;
  REQUIRE(reloaded.parse(meta.serialize()));
  CHECK(reloaded.collection_id() == "pack-uuid");
  CHECK(reloaded.collection_revision() == 4);
  CHECK(reloaded.in_collection());
}

TEST_CASE("ModMeta clear_collection untracks", "[collection][sync]") {
  engine::ModMeta meta;
  meta.set_collection_id("pack-uuid");
  meta.set_collection_revision(4);
  meta.set_in_collection(true);
  meta.clear_collection();
  CHECK(meta.collection_id().empty());
  CHECK(meta.collection_revision() == 0);
  CHECK_FALSE(meta.in_collection());
}

TEST_CASE("ModMeta bad revision string reads as 0", "[collection][sync]") {
  engine::ModMeta meta;
  meta.set("GameModManager", "collection_revision", "not-a-number");
  CHECK(meta.collection_revision() == 0);
}
