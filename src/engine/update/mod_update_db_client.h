#pragma once

// =============================================================================
// ModUpdateDbClient - polls the GameModManager/ModUpdateData dataset for
// per-mod update info, replacing per-mod live LoversLab page fetches with
// a small per-game index + on-demand shard lookup. See ticket
// Workspace-1bv9 (parent Workspace-a33s) for the full contract.
//
// ENGINE LAYER:
//   Qt-free, lives in gmm_engine. The UI drives it via the process-wide
//   singleton and consumes `last_checked()` + `has_update()` to badge the
//   mod list. The LoversLab provider's `fetch_mod_info` (live scrape) is
//   still wired for an explicit per-mod "Refresh" button - this client
//   just takes over the default, silent, batch path.
//
// ARTIFACTS (raw.githubusercontent.com, guest, ETag/If-None-Match):
//   data/manifest.json                          - generated_at + shard list
//   data/index.<GAME>.json                      - {id: updated_iso} map
//   data/<lo>-<hi>.json                         - per-shard array of records
//
// GAME TAG MAPPING:
//   GMM uses long game_ids ("skyrimspecialedition"); the DB uses short
//   tags ("SE"). `db_game_for()` resolves via a built-in table for the
//   games the epic enumerates, with a per-game knowledge override
//   ("modupdate_db_game") so plugins can ship their own mapping without
//   touching the engine.
//
// COMPARE LOGIC (the heart of the badge):
//   `has_update(index, file_id, local_updated)` - pure function:
//     - id absent from the DB           -> false (not tracked)
//     - local_updated empty             -> true  (DB has data, we have none)
//     - both present                    -> true iff db_updated > local
//     - db_updated unparseable          -> false (avoid false-positives)
//
// POLLING TRIGGERS (caller-driven; this class is stateless about WHEN):
//   - App startup
//   - Manual "Check for updates" button
//   - 6h timer
//   The 6h timer and startup hook are the UI's job; this class is a
//   pull-on-demand cache.
//
// OFFLINE / FAILURE:
//   Network failure or 5xx with no on-disk cache -> PollResult{ok=false}.
//   Network failure with an on-disk cache        -> PollResult{ok=true,
//                                                     from_cache=true,
//                                                     last_error=<why>}.
//   Per ticket: silent fallback, no error toast; UI shows "last checked:
//   <ts>" greyed when last_checked() predates a poll.

#include "engine/game/registry/game_knowledge.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::update
{

// One record extracted from a shard slice (the on-demand detail fetch when
// the UI wants version/tags for an "update available" mod).
struct ModUpdateShardRecord
{
  std::int64_t file_id = 0;
  std::string updated;   // ISO 8601 (Z-normalized)
  std::string version;   // softwareVersion / mods-version
  std::string category;  // applicationCategory
  std::vector<std::string> tags;
  // True when the shard slice contained the requested id. False when the
  // shard is missing (404, network) or the id is not in this shard (the
  // caller passed a bad id or the mod was deleted from the DB).
  bool found = false;
};

// In-memory representation of a per-game index fetched from the DB. The map
// is sparse - only mods the DB has ever seen have an entry.
struct ModUpdateIndex
{
  std::string game;  // ModUpdateData game tag (e.g. "SE")
  std::string etag;  // last ETag seen (used for the next If-None-Match)
  std::chrono::system_clock::time_point fetched_at{};
  // id -> updated ISO 8601 string. Compared lexicographically: the
  // scraper normalizes timestamps to ...Z, so lexicographic == chronological.
  std::unordered_map<std::int64_t, std::string> updated_by_id;
};

// Result of a poll: ok=true means we have a usable in-memory + on-disk
// index (either freshly fetched or served from the cache). ok=false means
// the fetch failed AND the cache was empty. last_error carries the network
// reason; the UI can show it on hover (Debug panel "Network" tab already
// shows the raw ring entry).
//
// The `body` + `etag` fields are populated by fetch_index_impl when ok=true
// and not_modified=false; the base class uses them to write through to
// disk and update the in-memory state. They are zero-length on 304 and on
// network failure (the base class then falls back to the on-disk cache).
struct PollResult
{
  bool ok = false;
  std::string last_error;
  // True when we served the on-disk cache because the network fetch
  // failed (or because If-None-Match returned 304, which is "still
  // current" and counts as a no-op success).
  bool from_cache = false;
  // True when the server returned 304 Not Modified. Distinct from
  // from_cache: a 304 means the on-disk cache is current, but a 5xx
  // with a cache also returns from_cache=true.
  bool not_modified = false;
  // Populated by fetch_index_impl on 200 OK. Empty on 304 / failure.
  std::string body;
  // Populated by fetch_index_impl on 200 OK (extracted from the ETag
  // response header). Empty when the server did not send one.
  std::string etag;
};

// Polls the ModUpdateData dataset for per-game update info. Singleton
// (process-wide); tests call set_instance() to inject a fake. Thread-safe:
// the in-memory cache is guarded by a mutex, and the network facade
// serialises the libcurl calls.
class ModUpdateDbClient
{
public:
  // Base URL for the ModUpdateData GitHub Pages artifact. raw.githubusercontent
  // is guest, CORS-friendly, and Pages-mirrored.
  static constexpr const char* kBaseUrl =
      "https://raw.githubusercontent.com/GameModManager/ModUpdateData/main";
  static constexpr const char* kIndexPrefix = "data/index";
  static constexpr const char* kShardPrefix = "data/";
  static constexpr const char* kIndexExt    = ".json";

  // Root of the per-game disk cache. Default = <data_dir>/modupdatedb
  // (data_dir is the parent of default_instances_dir()).
  [[nodiscard]] static std::filesystem::path cache_root();
  [[nodiscard]] static std::filesystem::path cache_dir_for(const std::string& game_id);

  // Map a GMM game_id (e.g. "skyrimspecialedition") to the ModUpdateData
  // game tag (e.g. "SE"). The default table is hand-rolled for the games
  // the epic enumerates; plugins can register overrides via
  // GameKnowledge ("modupdate_db_game" key, raw tag, e.g. "FO4").
  // Returns empty when the GMM game_id has no DB game - the caller
  // should skip polling for that game.
  [[nodiscard]] static std::string
  db_game_for(const std::string& gmm_game_id, const GameKnowledge* knowledge = nullptr);

  // Pure compare (no I/O). See class header for the truth table.
  [[nodiscard]] static bool has_update(const ModUpdateIndex& index,
                                       std::int64_t file_id,
                                       const std::string& local_updated);

  // Construct the shard filename for a given file_id. floor(id/1000) *
  // 1000 -> "<lo>-<hi>.json" (e.g. id=12345 -> "12000-12999.json").
  // Idiom: <lo> = (id / 1000) * 1000; <hi> = lo + 999.
  [[nodiscard]] static std::string shard_filename(std::int64_t file_id);

  // Construct the per-game index URL. db_game must be the short tag
  // ("SE"), NOT the GMM game_id; callers should go through db_game_for().
  [[nodiscard]] static std::string index_url(const std::string& db_game);

  // Construct the shard URL for a file_id. db_game is unused: shards
  // are global across DB games (by_game == false in the manifest).
  [[nodiscard]] static std::string
  shard_url([[maybe_unused]] const std::string& db_game, std::int64_t file_id);

  // --- Pure parsers (testable without the network) ---

  // Parse a per-game index JSON body. The DB shape is
  //   { "game": "SE", "updated": { "<id>": "<iso>" } }
  // (loose shape: extra keys ignored; missing keys default to empty).
  // Returns a populated ModUpdateIndex on success. On parse failure
  // returns a default-constructed one with `game` set to the
  // expected tag so the caller can still log/display.
  [[nodiscard]] static ModUpdateIndex parse_index(const std::string& body,
                                                  const std::string& expected_game);

  // Pull the single record matching file_id from a shard slice body.
  // The shard is a JSON array of objects, each carrying `id`, `updated`,
  // `version`, `category`, `tags` (last three optional).
  [[nodiscard]] static ModUpdateShardRecord parse_shard_record(const std::string& body,
                                                               std::int64_t file_id);

  // --- I/O ---

  // Pull the per-game index. Uses If-None-Match (last etag stored on
  // disk under <cache_dir>/etag) so an unchanged index 304s and we
  // keep using the on-disk copy. On success, the in-memory state is
  // refreshed and the on-disk index.json is rewritten.
  //   - On network failure (offline, DNS, 5xx, timeout) AND empty cache
  //     -> returns ok=false with last_error set.
  //   - On network failure with a previous on-disk cache
  //     -> returns ok=true, from_cache=true, last_error set.
  //   - On 304 with a previous cache -> ok=true, not_modified=true,
  //     from_cache=true, last_error empty (the silent-success path).
  //   - On 200 with body -> parses, stores, returns ok=true.
  // Never throws; callers run it on a worker.
  [[nodiscard]] PollResult fetch_index(const std::string& gmm_game_id);

  // Fetch the single shard slice containing file_id (floor(id/1000)).
  // Used on demand when the UI wants version/tags for an "update
  // available" mod. Always hits the network - shards are small and we
  // don't pre-cache them (the index already tells us whether a mod is
  // updated; the shard is the "show me details" drill-down).
  [[nodiscard]] ModUpdateShardRecord fetch_shard_record(const std::string& gmm_game_id,
                                                        std::int64_t file_id);

  // Read-only access to the in-memory index (nullptr when the index
  // has never been fetched for this game).
  [[nodiscard]] const ModUpdateIndex* index(const std::string& gmm_game_id) const;

  // Last successful poll timestamp for the UI's "last checked: ..."
  // label. epoch when the game has never been polled.
  [[nodiscard]] std::chrono::system_clock::time_point
  last_checked(const std::string& gmm_game_id) const;

  // Drive a poll for every game in `game_ids` (the UI calls this on
  // startup, on the 6h timer, and on the manual "Check for updates"
  // button). Sequential to be polite; the per-game call already
  // serialises the libcurl work. Logs a single summary line at the
  // end (one DEBUG per game).
  void poll_all(const std::vector<std::string>& gmm_game_ids);

  // Process-wide singleton. Tests use set_instance() to inject a fake.
  [[nodiscard]] static ModUpdateDbClient& instance();
  static void set_instance(std::unique_ptr<ModUpdateDbClient> c);

  virtual ~ModUpdateDbClient() = default;

  // Test seams: the production implementation talks to network::instance();
  // fakes override these to return canned data. Kept virtual on the base
  // so a test can `new FakeModUpdateDbClient` and set it without
  // subclassing the production class.
  [[nodiscard]] virtual PollResult
  fetch_index_impl(const std::string& db_game,
                   const std::string& if_none_match_etag) = 0;
  [[nodiscard]] virtual ModUpdateShardRecord
  fetch_shard_record_impl(const std::string& db_game, std::int64_t file_id) = 0;

protected:
  ModUpdateDbClient() = default;

  // Per-game state held under mu_.
  struct GameState
  {
    ModUpdateIndex index;
    std::chrono::system_clock::time_point last_checked{};
    std::string last_etag;  // saved for the next If-None-Match
    bool loaded = false;
  };

  // Helpers used by the production class. Made static so subclasses
  // (fakes) can reuse the disk persistence without instantiating the
  // base's state.
  [[nodiscard]] static std::string load_disk_etag(const std::filesystem::path& dir);
  static void save_disk_etag(const std::filesystem::path& dir, const std::string& etag);
  [[nodiscard]] static std::string load_disk_index(const std::filesystem::path& dir);
  static void save_disk_index(const std::filesystem::path& dir,
                              const std::string& body);
  void apply_fetched_index(const std::string& gmm_game_id, const std::string& db_game,
                           const std::string& body, const std::string& etag,
                           bool from_cache);

  mutable std::mutex mu_;
  std::unordered_map<std::string, GameState> states_;
};

// Production implementation: talks to network::instance(). Tests inject a
// fake via set_instance() or by deriving and overriding the *_impl hooks.
class NetworkModUpdateDbClient : public ModUpdateDbClient
{
public:
  NetworkModUpdateDbClient()           = default;
  ~NetworkModUpdateDbClient() override = default;

  [[nodiscard]] PollResult
  fetch_index_impl(const std::string& db_game,
                   const std::string& if_none_match_etag) override;
  [[nodiscard]] ModUpdateShardRecord
  fetch_shard_record_impl(const std::string& db_game, std::int64_t file_id) override;

  // Extract the ETag value from a raw "Header-Name: value\r\n" block.
  // Case-insensitive on the header name; trims surrounding whitespace.
  // Empty when the header is absent.
  [[nodiscard]] static std::string extract_etag(const std::string& response_headers);
};

}  // namespace engine::update
