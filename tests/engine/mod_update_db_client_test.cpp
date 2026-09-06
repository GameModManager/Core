// Tests for the engine::update::ModUpdateDbClient. Covers the pure
// pieces (URL/filename construction, ETag extraction, has_update truth
// table, JSON parsing) plus the disk cache + network-gateway round trip
// (a FakeModUpdateDbClient subclass that returns canned bodies, just
// like FakeNetworkManager does for network_test). LoversLab is
// untouched; this client is the default update-badge path.
#include "engine/update/mod_update_db_client.h"

#include "engine/core/instance/instance_utils.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

using engine::update::ModUpdateDbClient;
using engine::update::ModUpdateIndex;
using engine::update::ModUpdateShardRecord;
using engine::update::PollResult;

namespace
{

// RAII temp dir for disk-cache tests; rm -rf on scope exit. Layout
// matches the production data dir: <root>/instances is the instances
// dir, and <root>/modupdatedb is the per-game cache (a sibling of
// instances, the way masterlist_cache_dir() and icon_cache_dir() lay
// theirs out).
struct TempDir
{
  fs::path root;       // data root (parent of instances/)
  fs::path instances;  // <root>/instances
  explicit TempDir(const std::string& tag)
  {
    root      = fs::temp_directory_path() /
                ("gmm_mudb_test_" + tag + "_" + std::to_string(::getpid()) + "_" +
                 std::to_string(reinterpret_cast<std::uintptr_t>(this)));
    instances = root / "instances";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(instances);
  }
  ~TempDir()
  {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
  TempDir(const TempDir&)            = delete;
  TempDir& operator=(const TempDir&) = delete;
};

// Wire the instance-utils override so cache_root() / cache_dir_for() land
// in our temp dir. RAII restores the prior override on exit.
struct ScopedInstancesDir
{
  explicit ScopedInstancesDir(const fs::path& instances_dir)
  {
    engine::set_instances_dir_override(instances_dir);
  }
  ~ScopedInstancesDir() { engine::set_instances_dir_override({}); }
  ScopedInstancesDir(const ScopedInstancesDir&)            = delete;
  ScopedInstancesDir& operator=(const ScopedInstancesDir&) = delete;
};

// Canned response for the next fetch_index_impl call (FIFO). When the
// list is empty, fetch_index_impl returns ok=false with the supplied
// error message. Lets the test stage success, 304, 5xx, and offline
// scenarios without ever touching libcurl.
struct CannedIndexResponse
{
  int http_code = 200;
  std::string body;
  std::string etag;
  std::string error_message;  // when set, the response is "fail"
};

// In-memory shard record for the next fetch_shard_record_impl call
// (FIFO). Empty list -> found=false.
struct CannedShardRecord
{
  bool set = false;
  ModUpdateShardRecord rec;
};

// Test fake: serves a queue of canned responses so we exercise the
// fetch_index / fetch_shard_record / cache / state flow without the
// network. The base class owns the disk-cache write-through; we only
// answer the *_impl hooks.
class FakeModUpdateDbClient : public ModUpdateDbClient
{
public:
  std::vector<CannedIndexResponse> index_queue;
  std::vector<CannedShardRecord> shard_queue;
  // Cached etag the fake was asked for on the last fetch_index_impl
  // call (lets the test assert If-None-Match wiring).
  std::string last_if_none_match;

  PollResult fetch_index_impl(const std::string& /*db_game*/,
                              const std::string& if_none_match_etag) override
  {
    last_if_none_match = if_none_match_etag;
    PollResult res;
    if (index_queue.empty()) {
      res.last_error = "no canned response queued";
      return res;
    }
    const auto c = index_queue.front();
    index_queue.erase(index_queue.begin());
    if (!c.error_message.empty()) {
      res.last_error = c.error_message;
      return res;
    }
    if (c.http_code == 304) {
      res.ok           = true;
      res.not_modified = true;
      res.from_cache   = true;
      return res;
    }
    if (c.http_code != 200 || c.body.empty()) {
      res.last_error = "HTTP " + std::to_string(c.http_code);
      return res;
    }
    res.ok   = true;
    res.body = c.body;
    res.etag = c.etag;
    return res;
  }

  ModUpdateShardRecord fetch_shard_record_impl(const std::string& /*db_game*/,
                                               std::int64_t file_id) override
  {
    if (shard_queue.empty())
      return ModUpdateShardRecord{};
    const auto c = shard_queue.front();
    shard_queue.erase(shard_queue.begin());
    if (!c.set)
      return ModUpdateShardRecord{};
    ModUpdateShardRecord r = c.rec;
    if (r.file_id == 0)
      r.file_id = file_id;
    r.found = true;
    return r;
  }
};

}  // namespace

// --- URL + filename construction ------------------------------------------

TEST_CASE("mod_update_db: shard_filename uses floor(id/1000)*1000", "[engine][update]")
{
  REQUIRE(ModUpdateDbClient::shard_filename(0) == "0-999.json");
  REQUIRE(ModUpdateDbClient::shard_filename(1) == "0-999.json");
  REQUIRE(ModUpdateDbClient::shard_filename(999) == "0-999.json");
  REQUIRE(ModUpdateDbClient::shard_filename(1000) == "1000-1999.json");
  REQUIRE(ModUpdateDbClient::shard_filename(12345) == "12000-12999.json");
  // Negative ids clamp to 0..999 (defensive; the DB only stores positive
  // ids and the caller validates before calling).
  REQUIRE(ModUpdateDbClient::shard_filename(-7) == "0-999.json");
}

TEST_CASE("mod_update_db: index_url + shard_url target raw.githubusercontent",
          "[engine][update]")
{
  REQUIRE(ModUpdateDbClient::index_url("SE") ==
          "https://raw.githubusercontent.com/GameModManager/"
          "ModUpdateData/main/data/index.SE.json");
  REQUIRE(ModUpdateDbClient::shard_url("FO4", 12345) ==
          "https://raw.githubusercontent.com/GameModManager/"
          "ModUpdateData/main/data/12000-12999.json");
}

// --- db_game_for mapping ---------------------------------------------------

TEST_CASE("mod_update_db: db_game_for maps the epic game set", "[engine][update]")
{
  REQUIRE(ModUpdateDbClient::db_game_for("skyrimspecialedition") == "SE");
  REQUIRE(ModUpdateDbClient::db_game_for("skyrim") == "LE");
  REQUIRE(ModUpdateDbClient::db_game_for("fallout4") == "FO4");
  REQUIRE(ModUpdateDbClient::db_game_for("starfield") == "SF");
  REQUIRE(ModUpdateDbClient::db_game_for("oblivion") == "OB");
  REQUIRE(ModUpdateDbClient::db_game_for("oblivionremastered") == "OBR");
  REQUIRE(ModUpdateDbClient::db_game_for("sims4") == "S4");
  REQUIRE(ModUpdateDbClient::db_game_for("sims3") == "S3");
  // Unknown / empty -> empty (caller skips polling for that game).
  REQUIRE(ModUpdateDbClient::db_game_for("nosuchgame").empty());
  REQUIRE(ModUpdateDbClient::db_game_for("").empty());
}

TEST_CASE("mod_update_db: knowledge override wins over the default map",
          "[engine][update]")
{
  engine::GameKnowledge k;
  k.set("customgame", "modupdate_db_game", "ZZ");
  REQUIRE(ModUpdateDbClient::db_game_for("customgame", &k) == "ZZ");
  // Without the override, "customgame" is unknown.
  REQUIRE(ModUpdateDbClient::db_game_for("customgame").empty());
  // Override also wins over a known mapping.
  k.set("skyrim", "modupdate_db_game", "SKYRIM_OLD");
  REQUIRE(ModUpdateDbClient::db_game_for("skyrim", &k) == "SKYRIM_OLD");
  // No knowledge -> the default still applies.
  REQUIRE(ModUpdateDbClient::db_game_for("skyrim") == "LE");
}

// --- has_update truth table -----------------------------------------------

TEST_CASE("mod_update_db: has_update truth table", "[engine][update]")
{
  ModUpdateIndex idx;
  idx.game          = "SE";
  idx.updated_by_id = {
      {1234, "2025-06-05T14:23:11Z"},
      {5678, "2025-05-01T00:00:00Z"},
      // empty timestamp -> not flaggable
      {9999, ""},
  };

  SECTION("id missing from the DB -> false (not tracked)")
  {
    REQUIRE_FALSE(ModUpdateDbClient::has_update(idx, 42, "2025-01-01T00:00:00Z"));
  }
  SECTION("local empty + db present -> true (assume stale)")
  {
    REQUIRE(ModUpdateDbClient::has_update(idx, 1234, ""));
  }
  SECTION("db > local -> true (update available)")
  {
    REQUIRE(ModUpdateDbClient::has_update(idx, 1234, "2025-01-01T00:00:00Z"));
  }
  SECTION("db < local -> false (user has the newer copy)")
  {
    REQUIRE_FALSE(ModUpdateDbClient::has_update(idx, 1234, "2026-01-01T00:00:00Z"));
  }
  SECTION("db == local -> false (equal)")
  {
    REQUIRE_FALSE(ModUpdateDbClient::has_update(idx, 1234, "2025-06-05T14:23:11Z"));
  }
  SECTION("day-only vs full ISO 8601 compares correctly")
  {
    // local is day-only ("2025-06-05"); db is full ("2025-06-05T14:23:11Z").
    // The full string is lexicographically greater, so db > local.
    REQUIRE(ModUpdateDbClient::has_update(idx, 1234, "2025-06-05"));
  }
  SECTION("cross-day day-only vs full: db earlier than local -> no badge")
  {
    // Regression: a size-first compare in compare_iso8601 misordered
    // this pair (shorter day-only was assumed earlier; in fact a
    // same-month earlier day with a time is earlier). The correct
    // answer: db (2025-06-04 23:59:59Z) is chronologically BEFORE
    // local (2025-06-05), so no update is available.
    ModUpdateIndex cross;
    cross.updated_by_id = {{7777, "2025-06-04T23:59:59Z"}};
    REQUIRE_FALSE(ModUpdateDbClient::has_update(cross, 7777, "2025-06-05"));
  }
  SECTION("trailing offset +00:00 is normalized to Z for compare")
  {
    // Local sources may emit "+00:00" instead of "Z" (legacy LL scrape).
    // Both timestamps refer to the same instant, so no badge.
    ModUpdateIndex plus;
    plus.updated_by_id = {{8888, "2025-06-05T14:23:11Z"}};
    REQUIRE_FALSE(
        ModUpdateDbClient::has_update(plus, 8888, "2025-06-05T14:23:11+00:00"));
  }
  SECTION("db timestamp empty -> false (cannot decide)")
  {
    REQUIRE_FALSE(ModUpdateDbClient::has_update(idx, 9999, "2025-01-01T00:00:00Z"));
  }
  SECTION("db timestamp unparseable -> false (no false positive)")
  {
    ModUpdateIndex bad;
    bad.updated_by_id = {{42, "not-a-date"}};
    REQUIRE_FALSE(ModUpdateDbClient::has_update(bad, 42, "2025-01-01T00:00:00Z"));
  }
  SECTION("local timestamp unparseable -> false (cmp == 0)")
  {
    // When local is garbage, the comparator returns 0 -> no badge.
    // The DB has a real timestamp; the user has nonsense; refuse to
    // misclassify rather than risking a false positive.
    REQUIRE_FALSE(ModUpdateDbClient::has_update(idx, 1234, "yesterday"));
  }
}

// --- JSON parsing ----------------------------------------------------------

TEST_CASE("mod_update_db: parse_index reads the per-game index shape",
          "[engine][update]")
{
  const std::string body = R"({
        "game": "SE",
        "schema_version": 2,
        "updated": {
            "11488": "2025-06-05T14:23:11Z",
            "22000": "2025-08-12T00:00:00Z",
            "not-a-number": "skipped",
            "0": "skipped",
            "-1": "skipped"
        }
    })";
  auto idx               = ModUpdateDbClient::parse_index(body, "SE");
  REQUIRE(idx.game == "SE");
  REQUIRE(idx.updated_by_id.size() == 2);
  REQUIRE(idx.updated_by_id.at(11488) == "2025-06-05T14:23:11Z");
  REQUIRE(idx.updated_by_id.at(22000) == "2025-08-12T00:00:00Z");
}

TEST_CASE("mod_update_db: parse_index tolerates missing keys", "[engine][update]")
{
  SECTION("empty body -> empty index, game = expected")
  {
    auto idx = ModUpdateDbClient::parse_index("", "SE");
    REQUIRE(idx.game == "SE");
    REQUIRE(idx.updated_by_id.empty());
  }
  SECTION("non-object body -> empty index")
  {
    auto idx = ModUpdateDbClient::parse_index("[]", "SE");
    REQUIRE(idx.game == "SE");
    REQUIRE(idx.updated_by_id.empty());
  }
  SECTION("malformed JSON -> empty index, no throw")
  {
    auto idx = ModUpdateDbClient::parse_index("{not json", "SE");
    REQUIRE(idx.game == "SE");
    REQUIRE(idx.updated_by_id.empty());
  }
  SECTION("object without `updated` key -> empty index")
  {
    auto idx = ModUpdateDbClient::parse_index(R"({"game":"SE"})", "SE");
    REQUIRE(idx.updated_by_id.empty());
  }
  SECTION("game key overrides the expected_game fallback")
  {
    auto idx = ModUpdateDbClient::parse_index(R"({"game":"FO4","updated":{}})", "SE");
    REQUIRE(idx.game == "FO4");
  }
}

TEST_CASE("mod_update_db: parse_shard_record finds the matching id", "[engine][update]")
{
  const std::string body = R"([
        {"id": 11488, "updated": "2025-06-05T14:23:11Z", "version": "1.0",
         "category": "Objects", "tags": ["gameplay", "adult"]},
        {"id": 11489, "updated": "2025-06-06T00:00:00Z", "version": "2.0"}
    ])";
  auto rec               = ModUpdateDbClient::parse_shard_record(body, 11488);
  REQUIRE(rec.found);
  REQUIRE(rec.file_id == 11488);
  REQUIRE(rec.updated == "2025-06-05T14:23:11Z");
  REQUIRE(rec.version == "1.0");
  REQUIRE(rec.category == "Objects");
  REQUIRE(rec.tags.size() == 2);
  REQUIRE(rec.tags[0] == "gameplay");
  REQUIRE(rec.tags[1] == "adult");
}

TEST_CASE("mod_update_db: parse_shard_record miss is found=false", "[engine][update]")
{
  SECTION("id not in the slice")
  {
    auto rec = ModUpdateDbClient::parse_shard_record(R"([{"id":1,"updated":"x"}])", 42);
    REQUIRE_FALSE(rec.found);
  }
  SECTION("non-array body")
  {
    auto rec = ModUpdateDbClient::parse_shard_record(R"({"id":42})", 42);
    REQUIRE_FALSE(rec.found);
  }
  SECTION("malformed JSON")
  {
    auto rec = ModUpdateDbClient::parse_shard_record("{not json", 42);
    REQUIRE_FALSE(rec.found);
  }
  SECTION("id can be a string of digits")
  {
    auto rec = ModUpdateDbClient::parse_shard_record(
        R"([{"id":"11488","updated":"2025-06-05T14:23:11Z"}])", 11488);
    REQUIRE(rec.found);
    REQUIRE(rec.updated == "2025-06-05T14:23:11Z");
  }
}

// --- ETag header extraction ------------------------------------------------

TEST_CASE("mod_update_db: extract_etag parses a CRLF header block", "[engine][update]")
{
  using engine::update::NetworkModUpdateDbClient;
  REQUIRE(NetworkModUpdateDbClient::extract_etag("ETag: W/\"abc123\"\r\n") ==
          "W/\"abc123\"");
  REQUIRE(NetworkModUpdateDbClient::extract_etag("etag: \"deadbeef\"\r\n") ==
          "\"deadbeef\"");
  REQUIRE(NetworkModUpdateDbClient::extract_etag("Server: GitHub\r\nETag: \"x\"\r\n") ==
          "\"x\"");
  REQUIRE(NetworkModUpdateDbClient::extract_etag("").empty());
  REQUIRE(NetworkModUpdateDbClient::extract_etag("Server: GitHub\r\n").empty());
  // Header with no value -> empty (caller treats as "no etag available").
  REQUIRE(NetworkModUpdateDbClient::extract_etag("ETag: \r\n").empty());
}

// --- Network round-trip via FakeModUpdateDbClient -----------------------

TEST_CASE("mod_update_db: fetch_index fetches, parses, persists, and "
          "exposes the index for has_update",
          "[engine][update]")
{
  TempDir tmp("fetch_200");
  ScopedInstancesDir scope(tmp.instances);

  FakeModUpdateDbClient db;
  db.index_queue.push_back(CannedIndexResponse{
      200, R"({"game":"SE","updated":{"1234":"2025-06-05T14:23:11Z"}})", "W/\"v1\"",
      ""});

  auto r = db.fetch_index("skyrimspecialedition");
  REQUIRE(r.ok);
  REQUIRE_FALSE(r.from_cache);
  REQUIRE_FALSE(r.not_modified);
  REQUIRE(r.last_error.empty());

  // If-None-Match was empty (no prior cache).
  REQUIRE(db.last_if_none_match.empty());

  // If-None-Match was empty (no prior cache).
  REQUIRE(db.last_if_none_match.empty());

  // In-memory index is now populated.
  const ModUpdateIndex* idx = db.index("skyrimspecialedition");
  REQUIRE(idx != nullptr);
  REQUIRE(idx->updated_by_id.at(1234) == "2025-06-05T14:23:11Z");

  // has_update is the contract the UI uses to badge the mod list.
  REQUIRE(db.has_update(*idx, 1234, "2025-01-01T00:00:00Z"));
  REQUIRE_FALSE(db.has_update(*idx, 1234, "2026-01-01T00:00:00Z"));
  REQUIRE_FALSE(db.has_update(*idx, 9999, ""));  // not tracked

  // On-disk cache + ETag persisted for next session.
  const auto cache_dir = ModUpdateDbClient::cache_dir_for("skyrimspecialedition");
  REQUIRE(fs::is_regular_file(cache_dir / "index.json"));
  REQUIRE(fs::is_regular_file(cache_dir / "etag"));
  std::ifstream etag_in(cache_dir / "etag");
  std::stringstream etag_buf;
  etag_buf << etag_in.rdbuf();
  REQUIRE(etag_buf.str() == "W/\"v1\"");

  // last_checked is now non-zero.
  REQUIRE(db.last_checked("skyrimspecialedition").time_since_epoch().count() > 0);
}

TEST_CASE("mod_update_db: 304 with prior cache keeps index + last_checked",
          "[engine][update]")
{
  TempDir tmp("fetch_304");
  ScopedInstancesDir scope(tmp.instances);

  // Seed the on-disk cache with a prior successful poll.
  {
    FakeModUpdateDbClient db;
    db.index_queue.push_back(CannedIndexResponse{
        200, R"({"game":"SE","updated":{"1234":"2025-06-05T14:23:11Z"}})", "W/\"v1\"",
        ""});
    REQUIRE(db.fetch_index("skyrimspecialedition").ok);
  }

  // Now a 304: the server says the cache is current. The index in
  // memory stays populated from the prior poll; last_checked updates;
  // from_cache and not_modified are both set.
  {
    FakeModUpdateDbClient db;
    db.index_queue.push_back(CannedIndexResponse{304, "", "", ""});
    auto r = db.fetch_index("skyrimspecialedition");
    REQUIRE(r.ok);
    REQUIRE(r.not_modified);
    REQUIRE(r.from_cache);
    // If-None-Match carried the prior etag.
    REQUIRE(db.last_if_none_match == "W/\"v1\"");

    const ModUpdateIndex* idx = db.index("skyrimspecialedition");
    REQUIRE(idx != nullptr);
    REQUIRE(idx->updated_by_id.at(1234) == "2025-06-05T14:23:11Z");
    REQUIRE(db.last_checked("skyrimspecialedition").time_since_epoch().count() > 0);
  }
}

TEST_CASE("mod_update_db: 304 with no prior cache returns ok but no index",
          "[engine][update]")
{
  TempDir tmp("fetch_304_nocache");
  ScopedInstancesDir scope(tmp.instances);

  FakeModUpdateDbClient db;
  db.index_queue.push_back(CannedIndexResponse{304, "", "", ""});
  auto r = db.fetch_index("skyrimspecialedition");
  REQUIRE(r.ok);
  REQUIRE(r.not_modified);
  // 304 implies the server's view of our cache is current; we just
  // don't have one. The UI shows "not tracked" until a 200 lands.
  REQUIRE(db.index("skyrimspecialedition") == nullptr);
  // last_checked still updates so the "last checked: ..." label
  // doesn't go stale.
  REQUIRE(db.last_checked("skyrimspecialedition").time_since_epoch().count() > 0);
}

TEST_CASE("mod_update_db: network failure with on-disk cache falls back",
          "[engine][update]")
{
  TempDir tmp("fetch_5xx_cache");
  ScopedInstancesDir scope(tmp.instances);

  // Seed a successful 200.
  {
    FakeModUpdateDbClient db;
    db.index_queue.push_back(CannedIndexResponse{
        200, R"({"game":"FO4","updated":{"7777":"2025-09-01T00:00:00Z"}})", "W/\"v1\"",
        ""});
    REQUIRE(db.fetch_index("fallout4").ok);
  }

  // Now a network failure (the impl returns ok=false with an error).
  {
    FakeModUpdateDbClient db;
    db.index_queue.push_back(CannedIndexResponse{0, "", "", "connection timed out"});
    auto r = db.fetch_index("fallout4");
    REQUIRE(r.ok);
    REQUIRE(r.from_cache);
    // The reason is preserved for the Debug panel.
    REQUIRE(r.last_error == "connection timed out");
    const ModUpdateIndex* idx = db.index("fallout4");
    REQUIRE(idx != nullptr);
    REQUIRE(idx->updated_by_id.at(7777) == "2025-09-01T00:00:00Z");
  }
}

TEST_CASE("mod_update_db: network failure with no cache returns ok=false",
          "[engine][update]")
{
  TempDir tmp("fetch_fail_nocache");
  ScopedInstancesDir scope(tmp.instances);

  FakeModUpdateDbClient db;
  db.index_queue.push_back(CannedIndexResponse{0, "", "", "DNS resolution failed"});
  auto r = db.fetch_index("fallout4");
  REQUIRE_FALSE(r.ok);
  REQUIRE_FALSE(r.from_cache);
  REQUIRE(r.last_error == "DNS resolution failed");
  REQUIRE(db.index("fallout4") == nullptr);
}

TEST_CASE("mod_update_db: unmapped game is silently skipped", "[engine][update]")
{
  TempDir tmp("unmapped");
  ScopedInstancesDir scope(tmp.instances);

  FakeModUpdateDbClient db;
  // No queued response - the test asserts we don't even ask the impl.
  auto r = db.fetch_index("nosuchgame");
  REQUIRE_FALSE(r.ok);
  REQUIRE(r.last_error.find("no DB game mapping") != std::string::npos);
  // The impl was never called.
  REQUIRE(db.index_queue.size() == 0);
}

TEST_CASE("mod_update_db: fetch_shard_record returns canned record", "[engine][update]")
{
  TempDir tmp("shard");
  ScopedInstancesDir scope(tmp.instances);

  FakeModUpdateDbClient db;
  CannedShardRecord r;
  r.set          = true;
  r.rec.file_id  = 12345;
  r.rec.updated  = "2025-09-01T00:00:00Z";
  r.rec.version  = "1.2.3";
  r.rec.category = "Gameplay";
  r.rec.tags     = {"core", "balance"};
  db.shard_queue.push_back(r);

  auto rec = db.fetch_shard_record("skyrimspecialedition", 12345);
  REQUIRE(rec.found);
  REQUIRE(rec.file_id == 12345);
  REQUIRE(rec.version == "1.2.3");
  REQUIRE(rec.category == "Gameplay");
  REQUIRE(rec.tags.size() == 2);
  REQUIRE(rec.tags[0] == "core");
  REQUIRE(rec.tags[1] == "balance");
}

TEST_CASE("mod_update_db: fetch_shard_record with bad id is a no-op",
          "[engine][update]")
{
  TempDir tmp("shard_badid");
  ScopedInstancesDir scope(tmp.instances);

  FakeModUpdateDbClient db;
  auto rec = db.fetch_shard_record("skyrimspecialedition", 0);
  REQUIRE_FALSE(rec.found);
  REQUIRE(db.shard_queue.empty());

  rec = db.fetch_shard_record("skyrimspecialedition", -1);
  REQUIRE_FALSE(rec.found);
  REQUIRE(db.shard_queue.empty());
}
