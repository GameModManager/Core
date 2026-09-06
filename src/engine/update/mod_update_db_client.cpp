#include "engine/update/mod_update_db_client.h"

#include "engine/core/instance/instance_utils.h"
#include "engine/core/log/logger.h"
#include "engine/network/network_manager.h"

#include <nlohmann/json.hpp>

#include <cassert>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

namespace fs = std::filesystem;

namespace engine::update
{

namespace
{

  // Default mapping of GMM game_id -> ModUpdateData short tag. The set is the
  // same as the GamesAllowed in the epic (Workspace-a33s, section 3).
  // Plugins override via the "modupdate_db_game" knowledge key.
  const std::unordered_map<std::string, std::string>& default_db_game_map()
  {
    static const std::unordered_map<std::string, std::string> kMap = {
        // Skyrim (SE + LE retained; VR/SSEAE not in DB).
        {"skyrimspecialedition", "SE"},
        {"skyrim", "LE"},
        // Fallout 4
        {"fallout4", "FO4"},
        // Starfield
        {"starfield", "SF"},
        // Oblivion (+ Remastered folded into OB per epic recommendation
        // unless the DB distinguishes; we keep OBR for clarity in case
        // the dataset ships a separate OBR index later).
        {"oblivion", "OB"},
        {"oblivionremastered", "OBR"},
        // The Sims 4 + 3
        {"sims4", "S4"},
        {"sims3", "S3"},
    };
    return kMap;
  }

  // Knowledge key plugins use to override the GMM->DB game tag mapping.
  constexpr const char* kDbGameKey = "modupdate_db_game";

  // Trivial case-insensitive ASCII comparator for the ETag header. The
  // spec says ETags are quoted strings, so case is technically significant
  // on the value, but most servers emit weak ETags (W/"...") whose case we
  // ignore for comparison purposes; we treat "If-None-Match: W/\"x\"" and
  // "If-None-Match: w/\"X\"" as equal. The compare for has_update is on
  // ISO 8601 strings, which ARE case-sensitive (Z must be uppercase).
  // This helper is only used for the ETag cache.
  bool iequals(const std::string& a, const std::string& b)
  {
    if (a.size() != b.size())
      return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
      if (std::tolower(static_cast<unsigned char>(a[i])) !=
          std::tolower(static_cast<unsigned char>(b[i])))
        return false;
    }
    return true;
  }

  bool starts_with_i(const std::string& s, const char* prefix)
  {
    const std::size_t plen = std::char_traits<char>::length(prefix);
    if (s.size() < plen)
      return false;
    for (std::size_t i = 0; i < plen; ++i) {
      if (std::tolower(static_cast<unsigned char>(s[i])) !=
          std::tolower(static_cast<unsigned char>(prefix[i])))
        return false;
    }
    return true;
  }

  // Trim ASCII whitespace. Pulled out of loverslab_provider.cpp's namespace;
  // kept local so this header doesn't depend on a private helper.
  std::string trim(const std::string& s)
  {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
      return {};
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
  }

  // Check that a string looks like a plausible ISO 8601 timestamp. The
  // scraper normalizes to "...Z" (UTC) form, so the only acceptable shapes
  // are YYYY-MM-DD and YYYY-MM-DDTHH:MM:SS[.fff][Z|+HH:MM]. We don't need
  // to be perfect - the goal is to avoid "compare a date with a non-date
  // string" false-positives in has_update.
  bool looks_like_iso8601(const std::string& s)
  {
    if (s.size() < 10)
      return false;
    auto is_digit = [](char c) {
      return c >= '0' && c <= '9';
    };
    for (int i = 0; i < 4; ++i)
      if (!is_digit(s[i]))
        return false;
    if (s[4] != '-')
      return false;
    if (!is_digit(s[5]) || !is_digit(s[6]))
      return false;
    if (s[7] != '-')
      return false;
    if (!is_digit(s[8]) || !is_digit(s[9]))
      return false;
    // Anything after the date is "best effort" - T, Z, +, -, digits. We
    // don't need to fully parse it.
    return true;
  }

  // Lexicographic compare of two ISO 8601 strings. Treats empty as "less
  // than anything" so the caller's truth table stays tidy. Returns
  //   -1 if a < b
  //    0 if a == b (or both unparseable, or one is empty and we consider
  //       them incomparable)
  //   +1 if a > b
  // "Unparseable" in this context means the string is non-empty but does
  // not look like a date - in which case we return 0 and the caller treats
  // it as "don't badge".
  // Strip a trailing "+HH:MM" or "-HH:MM" offset and replace with "Z"
  // so lex compare matches chrono compare. The DB always emits Z; some
  // local sources (legacy LoversLab scrape) emit "+00:00" instead. The
  // scraper-side contract is that local updated fields are also Z by the
  // time they reach this client, but we tolerate +00:00 defensively.
  std::string normalize_offset_to_z(std::string s)
  {
    // Minimum plausible timestamp with offset is 20 chars:
    // "YYYY-MM-DDTHH:MM:SS+HH:MM" (no fractional seconds).
    if (s.size() < 20)
      return s;
    const std::size_t pos = s.size() - 6;  // start of "+HH:MM" / "-HH:MM"
    const char c          = s[pos];
    if (c != '+' && c != '-')
      return s;
    // Must be exactly 5 chars after the sign: two digits, ':', two digits.
    for (std::size_t i = pos + 1; i < s.size(); ++i) {
      const char x        = s[i];
      const bool is_digit = x >= '0' && x <= '9';
      const bool is_colon = x == ':';
      if (i == pos + 3) {
        if (!is_colon)
          return s;
      } else if (!is_digit) {
        return s;
      }
    }
    s.resize(pos);
    s.push_back('Z');
    return s;
  }

  int compare_iso8601(const std::string& a, const std::string& b)
  {
    if (a.empty() && b.empty())
      return 0;
    if (a.empty())
      return -1;
    if (b.empty())
      return 1;
    if (!looks_like_iso8601(a) || !looks_like_iso8601(b))
      return 0;  // incomparable; has_update will refuse to flag
    // Lex compare on Z-normalized forms. We do NOT short-circuit on
    // size: a day-only "2025-06-05" is a prefix of the full form, so
    // the shorter one is naturally lex-smaller and the longer one is
    // naturally lex-greater. A size-only compare mis-orders cross-day
    // pairs like db="2025-06-04T23:59:59Z" vs local="2025-06-05".
    const std::string an = normalize_offset_to_z(a);
    const std::string bn = normalize_offset_to_z(b);
    if (an < bn)
      return -1;
    if (an > bn)
      return 1;
    return 0;
  }

}  // namespace

// --- Static helpers --------------------------------------------------------

fs::path ModUpdateDbClient::cache_root()
{
  return default_instances_dir().parent_path() / "modupdatedb";
}

fs::path ModUpdateDbClient::cache_dir_for(const std::string& gmm_game_id)
{
  // gmm_game_id becomes a single path component. Internal callers go
  // through db_game_for() which returns controlled tags ("SE", "FO4",
  // ...); a stray "/" or ".." would escape the cache root, so we
  // assert the invariant in debug builds.
  assert(!gmm_game_id.empty());
  assert(gmm_game_id.find('/') == std::string::npos);
  assert(gmm_game_id.find("..") == std::string::npos);
  return cache_root() / gmm_game_id;
}

std::string ModUpdateDbClient::db_game_for(const std::string& gmm_game_id,
                                           const GameKnowledge* knowledge)
{
  if (gmm_game_id.empty())
    return {};
  // Plugin override first - cheap, no I/O, takes precedence over the
  // engine default (lets a plugin remap a game to a new DB tag without
  // shipping engine changes).
  if (knowledge != nullptr) {
    const std::string override = knowledge->get(gmm_game_id, kDbGameKey, "");
    if (!override.empty())
      return override;
  }
  const auto& m = default_db_game_map();
  auto it       = m.find(gmm_game_id);
  if (it != m.end())
    return it->second;
  return {};
}

bool ModUpdateDbClient::has_update(const ModUpdateIndex& index, std::int64_t file_id,
                                   const std::string& local_updated)
{
  auto it = index.updated_by_id.find(file_id);
  if (it == index.updated_by_id.end())
    return false;  // not tracked
  const std::string& db_updated = it->second;
  if (db_updated.empty())
    return false;  // DB entry has no timestamp; can't decide
  if (local_updated.empty())
    return true;  // DB has data, user has none - assume stale
  const int cmp = compare_iso8601(db_updated, local_updated);
  // cmp == 0 covers "equal" AND "incomparable (unparseable)"; both
  // are conservative "no badge" so the user is not falsely alerted.
  return cmp > 0;
}

std::string ModUpdateDbClient::shard_filename(std::int64_t file_id)
{
  if (file_id < 0)
    file_id = 0;
  const std::int64_t lo = (file_id / 1000) * 1000;
  const std::int64_t hi = lo + 999;
  return std::to_string(lo) + "-" + std::to_string(hi) + ".json";
}

std::string ModUpdateDbClient::index_url(const std::string& db_game)
{
  return std::string(kBaseUrl) + "/" + kIndexPrefix + "." + db_game + kIndexExt;
}

std::string ModUpdateDbClient::shard_url([[maybe_unused]] const std::string& db_game,
                                         std::int64_t file_id)
{
  // Shards are global across DB games (by_game == false in the manifest),
  // so db_game does not participate in the URL. Kept in the signature
  // for symmetry with index_url and the abstract interface.
  return std::string(kBaseUrl) + "/" + kShardPrefix + shard_filename(file_id);
}

// --- Pure parsers ----------------------------------------------------------

ModUpdateIndex ModUpdateDbClient::parse_index(const std::string& body,
                                              const std::string& expected_game)
{
  ModUpdateIndex out;
  out.game = expected_game;
  if (body.empty())
    return out;
  try {
    auto j = nlohmann::json::parse(body);
    if (!j.is_object())
      return out;
    if (j.contains("game") && j["game"].is_string()) {
      const std::string g = j["game"].get<std::string>();
      if (!g.empty())
        out.game = g;
    }
    if (j.contains("updated") && j["updated"].is_object()) {
      for (auto it = j["updated"].begin(); it != j["updated"].end(); ++it) {
        if (!it.value().is_string())
          continue;
        const std::string& key = it.key();
        // id is a string of digits in the DB; parse defensively.
        std::int64_t id = 0;
        try {
          id = std::stoll(key);
        } catch (const std::exception&) {
          continue;
        }
        if (id <= 0)
          continue;
        out.updated_by_id.emplace(id, it.value().get<std::string>());
      }
    }
  } catch (const std::exception&) {
    // Malformed body: leave the index empty. The caller sees ok=true
    // (the fetch succeeded) but no updates. Better than a crash.
    out.updated_by_id.clear();
  }
  return out;
}

ModUpdateShardRecord ModUpdateDbClient::parse_shard_record(const std::string& body,
                                                           std::int64_t file_id)
{
  ModUpdateShardRecord out;
  if (body.empty())
    return out;
  try {
    auto j = nlohmann::json::parse(body);
    if (!j.is_array())
      return out;
    for (const auto& node : j) {
      if (!node.is_object())
        continue;
      std::int64_t id = 0;
      if (node.contains("id")) {
        if (node["id"].is_number_integer())
          id = node["id"].get<std::int64_t>();
        else if (node["id"].is_string()) {
          try {
            id = std::stoll(node["id"].get<std::string>());
          } catch (const std::exception&) {
            continue;
          }
        } else {
          continue;
        }
      } else {
        continue;
      }
      if (id != file_id)
        continue;
      out.file_id = id;
      out.found   = true;
      if (node.contains("updated") && node["updated"].is_string())
        out.updated = trim(node["updated"].get<std::string>());
      if (node.contains("version") && node["version"].is_string())
        out.version = trim(node["version"].get<std::string>());
      if (node.contains("category") && node["category"].is_string())
        out.category = trim(node["category"].get<std::string>());
      if (node.contains("tags") && node["tags"].is_array()) {
        for (const auto& t : node["tags"]) {
          if (t.is_string())
            out.tags.push_back(trim(t.get<std::string>()));
        }
      }
      return out;
    }
  } catch (const std::exception&) {
    // Malformed body: found stays false.
  }
  return out;
}

// --- Disk persistence -----------------------------------------------------

std::string ModUpdateDbClient::load_disk_etag(const fs::path& dir)
{
  std::error_code ec;
  if (!fs::is_regular_file(dir / "etag", ec) || ec)
    return {};
  std::ifstream ifs(dir / "etag");
  if (!ifs.is_open())
    return {};
  std::stringstream buf;
  buf << ifs.rdbuf();
  return trim(buf.str());
}

void ModUpdateDbClient::save_disk_etag(const fs::path& dir, const std::string& etag)
{
  std::error_code ec;
  fs::create_directories(dir, ec);
  std::ofstream ofs(dir / "etag", std::ios::binary | std::ios::trunc);
  if (!ofs.is_open())
    return;
  ofs << etag;
}

std::string ModUpdateDbClient::load_disk_index(const fs::path& dir)
{
  std::error_code ec;
  if (!fs::is_regular_file(dir / "index.json", ec) || ec)
    return {};
  std::ifstream ifs(dir / "index.json", std::ios::binary);
  if (!ifs.is_open())
    return {};
  std::stringstream buf;
  buf << ifs.rdbuf();
  return buf.str();
}

void ModUpdateDbClient::save_disk_index(const fs::path& dir, const std::string& body)
{
  std::error_code ec;
  fs::create_directories(dir, ec);
  std::ofstream ofs(dir / "index.json", std::ios::binary | std::ios::trunc);
  if (!ofs.is_open())
    return;
  ofs << body;
}

void ModUpdateDbClient::apply_fetched_index(const std::string& gmm_game_id,
                                            const std::string& db_game,
                                            const std::string& body,
                                            const std::string& etag, bool from_cache)
{
  auto parsed       = parse_index(body, db_game);
  parsed.etag       = etag;
  parsed.fetched_at = std::chrono::system_clock::now();
  const auto dir    = cache_dir_for(gmm_game_id);
  std::lock_guard<std::mutex> lock(mu_);
  auto& st        = states_[gmm_game_id];
  st.index        = std::move(parsed);
  st.last_etag    = etag;
  st.last_checked = std::chrono::system_clock::now();
  st.loaded       = true;
  (void)from_cache;  // state.markers are uniform; from_cache is for the
                     // PollResult the caller already decided on
}

// --- In-memory accessors ---------------------------------------------------

const ModUpdateIndex* ModUpdateDbClient::index(const std::string& gmm_game_id) const
{
  std::lock_guard<std::mutex> lock(mu_);
  auto it = states_.find(gmm_game_id);
  if (it == states_.end() || !it->second.loaded)
    return nullptr;
  return &it->second.index;
}

std::chrono::system_clock::time_point
ModUpdateDbClient::last_checked(const std::string& gmm_game_id) const
{
  std::lock_guard<std::mutex> lock(mu_);
  auto it = states_.find(gmm_game_id);
  if (it == states_.end())
    return {};
  return it->second.last_checked;
}

// --- Polling ---------------------------------------------------------------

PollResult ModUpdateDbClient::fetch_index(const std::string& gmm_game_id)
{
  PollResult res;
  if (gmm_game_id.empty()) {
    res.last_error = "empty game_id";
    return res;
  }
  const std::string db_game = db_game_for(gmm_game_id, nullptr);
  if (db_game.empty()) {
    res.last_error = "no DB game mapping for " + gmm_game_id;
    return res;
  }

  const auto dir = cache_dir_for(gmm_game_id);
  // Use the on-disk etag (last successful fetch) as If-None-Match. The
  // in-memory copy is the same string for the duration of a session,
  // but the on-disk copy survives restarts.
  const std::string etag        = load_disk_etag(dir);
  const std::string cached_body = load_disk_index(dir);

  const PollResult impl = fetch_index_impl(db_game, etag);

  if (impl.ok) {
    if (impl.not_modified) {
      // 304: server says our cache is still current. Refresh the
      // last_checked timestamp so the UI's greyed label updates,
      // but don't rewrite the body on disk. If we don't have an
      // in-memory index yet, populate it from the on-disk cache.
      if (!cached_body.empty()) {
        apply_fetched_index(gmm_game_id, db_game, cached_body, etag, true);
      } else {
        std::lock_guard<std::mutex> lock(mu_);
        auto& st        = states_[gmm_game_id];
        st.last_checked = std::chrono::system_clock::now();
      }
      res.ok           = true;
      res.from_cache   = true;
      res.not_modified = true;
      return res;
    }
    // 200 OK with body. Write through to disk and parse.
    if (!impl.body.empty()) {
      save_disk_index(dir, impl.body);
      if (!impl.etag.empty())
        save_disk_etag(dir, impl.etag);
      apply_fetched_index(gmm_game_id, db_game, impl.body, impl.etag, false);
      res.ok = true;
      return res;
    }
    // 200 with no body? Treat as failure; fall back to cache.
    res.last_error = "empty body";
    if (!cached_body.empty()) {
      apply_fetched_index(gmm_game_id, db_game, cached_body, etag, true);
      res.ok         = true;
      res.from_cache = true;
      return res;
    }
    return res;
  }

  // Network failed. Serve the disk cache silently if we have one.
  if (!cached_body.empty()) {
    apply_fetched_index(gmm_game_id, db_game, cached_body, etag, true);
    res.ok         = true;
    res.from_cache = true;
    // Keep the reason for the Debug panel; the UI label stays silent
    // per ticket ("last checked: ..." stays greyed, no error toast).
    res.last_error = impl.last_error;
    return res;
  }
  res.last_error = impl.last_error.empty()
                       ? std::string("network failure with no cache")
                       : impl.last_error;
  return res;
}

ModUpdateShardRecord
ModUpdateDbClient::fetch_shard_record(const std::string& gmm_game_id,
                                      std::int64_t file_id)
{
  ModUpdateShardRecord out;
  if (gmm_game_id.empty() || file_id <= 0)
    return out;
  const std::string db_game = db_game_for(gmm_game_id, nullptr);
  if (db_game.empty())
    return out;
  return fetch_shard_record_impl(db_game, file_id);
}

void ModUpdateDbClient::poll_all(const std::vector<std::string>& gmm_game_ids)
{
  int ok = 0, cached = 0, failed = 0;
  for (const auto& gid : gmm_game_ids) {
    const PollResult r = fetch_index(gid);
    if (!r.ok) {
      ++failed;
      continue;
    }
    if (r.from_cache)
      ++cached;
    else
      ++ok;
  }
  Logger::instance().debug("ModUpdateDbClient: poll_all done - " + std::to_string(ok) +
                           " fresh, " + std::to_string(cached) + " from cache, " +
                           std::to_string(failed) + " failed across " +
                           std::to_string(gmm_game_ids.size()) + " games");
}

// --- Singleton -------------------------------------------------------------

namespace
{
  std::unique_ptr<ModUpdateDbClient> g_instance;
}

ModUpdateDbClient& ModUpdateDbClient::instance()
{
  if (!g_instance)
    g_instance = std::make_unique<NetworkModUpdateDbClient>();
  return *g_instance;
}

void ModUpdateDbClient::set_instance(std::unique_ptr<ModUpdateDbClient> c)
{
  g_instance = std::move(c);
}

// --- Network implementation -----------------------------------------------

std::string NetworkModUpdateDbClient::extract_etag(const std::string& response_headers)
{
  if (response_headers.empty())
    return {};
  // Walk line-by-line. response_headers uses "\r\n" separators; the
  // header name is case-insensitive.
  std::size_t pos = 0;
  while (pos < response_headers.size()) {
    std::size_t eol = response_headers.find("\r\n", pos);
    if (eol == std::string::npos)
      eol = response_headers.size();
    const std::string line = response_headers.substr(pos, eol - pos);
    pos                    = eol + 2;
    if (line.empty())
      continue;
    const auto colon = line.find(':');
    if (colon == std::string::npos)
      continue;
    const std::string name = line.substr(0, colon);
    if (!iequals(name, "etag"))
      continue;
    std::string value = line.substr(colon + 1);
    value             = trim(value);
    if (!value.empty())
      return value;
  }
  return {};
}

PollResult
NetworkModUpdateDbClient::fetch_index_impl(const std::string& db_game,
                                           const std::string& if_none_match_etag)
{
  PollResult res;
  network::Request req;
  req.url             = ModUpdateDbClient::index_url(db_game);
  req.caller          = NET_CALLER;
  req.timeout         = std::chrono::seconds(15);
  req.follow_redirect = true;
  // ModUpdateData index files are < 1MB. Cap defensively.
  req.max_bytes = 4 * 1024 * 1024;
  if (!if_none_match_etag.empty()) {
    req.headers.push_back("If-None-Match: " + if_none_match_etag);
  }
  req.headers.push_back("User-Agent: GameModManager/" + std::string(VERSION) +
                        " (ModUpdateData poller)");

  auto resp = network::instance().request(req);
  if (!resp.error.empty()) {
    res.last_error = resp.error;
    return res;
  }
  if (resp.http_code == 304) {
    res.ok           = true;
    res.not_modified = true;
    res.from_cache   = true;
    return res;
  }
  if (resp.http_code != 200) {
    res.last_error = "HTTP " + std::to_string(resp.http_code);
    return res;
  }
  if (resp.body.empty()) {
    res.last_error = "empty body";
    return res;
  }
  res.ok   = true;
  res.body = std::move(resp.body);
  res.etag = extract_etag(resp.response_headers);
  return res;
}

ModUpdateShardRecord
NetworkModUpdateDbClient::fetch_shard_record_impl(const std::string& db_game,
                                                  std::int64_t file_id)
{
  ModUpdateShardRecord out;
  network::Request req;
  req.url             = ModUpdateDbClient::shard_url(db_game, file_id);
  req.caller          = NET_CALLER;
  req.timeout         = std::chrono::seconds(20);
  req.follow_redirect = true;
  // Shards are < 1MB on disk; cap at 4MB to bound hostile servers.
  req.max_bytes = 4 * 1024 * 1024;
  req.headers.push_back("User-Agent: GameModManager/" + std::string(VERSION) +
                        " (ModUpdateData poller)");

  auto resp = network::instance().request(req);
  if (!resp.error.empty())
    return out;
  if (resp.http_code != 200)
    return out;
  if (resp.body.empty())
    return out;
  return ModUpdateDbClient::parse_shard_record(resp.body, file_id);
}

}  // namespace engine::update
