#include "engine/source/nexus/update_check.h"

#include "engine/log/logger.h"
#include "engine/mod/meta/mod_meta.h"
#include "engine/source/nexus/account.h"
#include "engine/source/nexus/http.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <set>

namespace engine::Source::Nexus::UpdateCheck {

namespace {

  // meta.ini keys. `lastnexusquery` is MO2's own key and is already in
  // mod_meta.cpp's kNexusFields set, so an instance imported from MO2 arrives
  // with its cached check time rather than looking freshly unchecked.
  constexpr const char *kSecNexus     = "Nexusmods";
  constexpr const char *kSecGeneral   = "General";
  constexpr const char *kKeyModId     = "modid";
  constexpr const char *kKeyFileId    = "fileid";
  constexpr const char *kKeyLastQuery = "lastnexusquery";
  constexpr const char *kKeyNewest    = "newestversion";
  constexpr const char *kKeyIgnored   = "ignoredversion";
  // The engine-owned verdict. A separate key from newestversion on purpose:
  // newestversion is display text a user can edit, while this answers the one
  // question the UI must not second-guess - does an update exist right now.
  constexpr const char *kKeyUpdate = "nexus_update";

  constexpr int64_t kDay   = 86400;
  constexpr int64_t kMonth = 30 * kDay;

  std::string utc_iso8601(int64_t unix_seconds) {
    const auto tt = static_cast<std::time_t>(unix_seconds);
    std::tm tm{};
    gmtime_r(&tt, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
  }

  int64_t parse_iso8601(const std::string &text) {
    if (text.empty())
      return 0;
    std::tm tm{};
    if (std::sscanf(text.c_str(), "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon,
                    &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6)
      return 0;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    return static_cast<int64_t>(timegm(&tm));
  }

  int64_t to_i64(const std::string &s) {
    try {
      return s.empty() ? 0 : std::stoll(s);
    } catch (...) {
      return 0;
    }
  }

  // One request, one outcome. `ok` false means the answer is UNKNOWN, which is
  // never the same as an empty answer.
  struct HttpReply {
    bool ok        = false;
    bool refused   = false;  // 429 / 403: the key is out of budget
    long http_code = 0;
    std::string body;
  };

  HttpReply nexus_get(const std::string &url, const std::string &api_key) {
    HttpReply reply;
    curl_slist *headers = nullptr;
    headers             = curl_slist_append(headers, ("apikey: " + api_key).c_str());
    headers             = curl_slist_append(headers, "Accept: application/json");

    std::string resp_headers;
    const bool transport = Http::nexus_http_request(
        url, "", reply.body, reply.http_code, headers, &resp_headers);
    curl_slist_free_all(headers);

    if (resp_headers.size() > 20)  // sanity check - don't parse empty/trivial
      Account::parse_rate_limits(resp_headers);

    if (reply.http_code == 429 || reply.http_code == 403) {
      reply.refused = true;
      return reply;
    }
    reply.ok = transport && reply.http_code == 200;
    return reply;
  }

  // The file list of one mod: the ids Nexus still serves plus the version string
  // of the newest of them. The version rides along because that is what
  // `newestversion` displays and what `ignoredversion` has to match - asking
  // Nexus for it separately would be a second request for one string.
  struct FileInfo {
    bool ok = false;
    std::vector<long long> available;
    std::string newest_version;
  };

  FileInfo parse_files(const std::string &body) {
    FileInfo info;
    try {
      auto j = nlohmann::json::parse(body);
      if (!j.contains("data") || !j["data"].is_object())
        return info;
      const auto &data = j["data"];

      // "available_mod_files" is exactly the downloadable set - archived and
      // deleted files are left out. The `files` fallback keeps a mod readable
      // if Nexus ever drops the field; it may then include archived entries,
      // which errs toward calling an update rather than hiding one.
      if (data.contains("available_mod_files") &&
          data["available_mod_files"].is_array()) {
        for (const auto &id : data["available_mod_files"])
          if (id.is_number_integer() && id.get<long long>() > 0)
            info.available.push_back(id.get<long long>());
      } else if (data.contains("files") && data["files"].is_array()) {
        for (const auto &f : data["files"])
          if (f.is_object() && f.value("id", 0LL) > 0)
            info.available.push_back(f["id"].get<long long>());
      }

      if (!info.available.empty() && data.contains("files") &&
          data["files"].is_array()) {
        const long long newest =
            *std::max_element(info.available.begin(), info.available.end());
        for (const auto &f : data["files"]) {
          if (f.is_object() && f.value("id", 0LL) == newest) {
            info.newest_version = f.value("version", std::string{});
            break;
          }
        }
      }
      info.ok = !info.available.empty();
    } catch (const std::exception &e) {
      Logger::instance().error(
          std::string("[NexusUpdateCheck] file list parse failed: ") + e.what());
    }
    return info;
  }

  // MO2 writes newestVersion in camel case; GMM's Mod Info panel has always
  // written the lowercase spelling. An instance imported from MO2 therefore
  // carries the camel one, so read both rather than treating an imported mod as
  // having no known newest version - which would report a spurious update.
  std::string read_newest_version(const ModMeta &meta) {
    std::string v = meta.get(kSecGeneral, kKeyNewest);
    if (v.empty())
      v = meta.get(kSecGeneral, "newestVersion");
    return v;
  }

  // Every Nexus-sourced mod in the instance, read off its own meta.ini, plus
  // the oldest `lastnexusquery` among them (0 when none was ever checked).
  std::vector<InstalledMod> read_installed(const std::filesystem::path &mods_dir,
                                           int64_t &oldest_checked) {
    std::vector<InstalledMod> out;
    oldest_checked = 0;
    std::error_code ec;
    if (!std::filesystem::is_directory(mods_dir, ec))
      return out;

    for (const auto &entry : std::filesystem::directory_iterator(mods_dir, ec)) {
      if (ec)
        return out;
      if (!entry.is_directory())
        continue;
      const std::string folder = entry.path().filename().string();
      const auto meta          = ModMeta::load(mods_dir, folder);
      if (meta.source_type() != "nexus")
        continue;

      InstalledMod mod;
      mod.folder            = folder;
      mod.mod_id            = to_i64(meta.get(kSecNexus, kKeyModId));
      mod.installed_file_id = to_i64(meta.get(kSecNexus, kKeyFileId));
      mod.last_checked      = parse_iso8601(meta.get(kSecNexus, kKeyLastQuery));
      mod.newest_version    = read_newest_version(meta);
      mod.ignored_version   = meta.get(kSecGeneral, kKeyIgnored);
      if (mod.mod_id <= 0)
        continue;
      out.push_back(std::move(mod));
    }

    for (const auto &mod : out) {
      if (mod.last_checked > 0 &&
          (oldest_checked == 0 || mod.last_checked < oldest_checked))
        oldest_checked = mod.last_checked;
    }
    return out;
  }

  // Re-stamp a mod we decided from the cache. Without this the stored time
  // never advances, the chosen window keeps growing past the point it can
  // actually cover, and updates get silently skipped - the failure this design
  // cannot have. One small INI write, once a sweep, per Nexus mod.
  bool stamp(const std::filesystem::path &mods_dir, const std::string &folder,
             int64_t now) {
    auto meta = ModMeta::load(mods_dir, folder);
    meta.set(kSecNexus, kKeyLastQuery, utc_iso8601(now));
    if (!meta.save(mods_dir, folder))
      Logger::instance().warn("[NexusUpdateCheck] could not write meta.ini for " +
                              folder);
    return true;
  }

}  // namespace

const char *period_query_value(Period p) {
  switch (p) {
  case Period::Day:
    return "1d";
  case Period::Week:
    return "1w";
  case Period::Month:
    return "1m";
  }
  return "1d";
}

// ---------------------------------------------------------------------------
// parse_updated
// ---------------------------------------------------------------------------

bool parse_updated(const std::string &body, std::vector<UpdatedMod> &out) {
  std::vector<UpdatedMod> parsed;
  try {
    auto j = nlohmann::json::parse(body);
    if (!j.is_array())
      return false;
    for (const auto &row : j) {
      if (!row.is_object() || !row.contains("mod_id"))
        continue;
      UpdatedMod m;
      m.mod_id = row.value("mod_id", 0LL);
      if (m.mod_id <= 0)
        continue;
      m.latest_file_update = row.value("latest_file_update", 0LL);
      parsed.push_back(m);
    }
  } catch (const std::exception &e) {
    Logger::instance().error(
        std::string("[NexusUpdateCheck] updated.json parse failed: ") + e.what());
    return false;
  }
  out = std::move(parsed);
  return true;
}

// ---------------------------------------------------------------------------
// evaluate / update_visible / full_recheck_required / due_for_query /
// period_for
// ---------------------------------------------------------------------------

UpdateState evaluate(long long installed_file_id,
                     const std::vector<long long> &available_file_ids) {
  UpdateState s;
  if (installed_file_id <= 0 || available_file_ids.empty())
    return s;  // Verdict::Unknown - never guess Update out of absent data
  s.newest_file_id =
      *std::max_element(available_file_ids.begin(), available_file_ids.end());
  s.state = s.newest_file_id > installed_file_id ? Verdict::Update : Verdict::Current;
  return s;
}

bool update_visible(bool available, std::string_view newest_version,
                    std::string_view ignored_version) {
  if (!available)
    return false;
  return ignored_version.empty() || ignored_version != newest_version;
}

bool full_recheck_required(int64_t seconds_since_oldest_check) {
  return seconds_since_oldest_check <= 0 || seconds_since_oldest_check > kMonth;
}

std::vector<InstalledMod> due_for_query(const std::vector<InstalledMod> &installed,
                                        const std::vector<UpdatedMod> &updated,
                                        bool full_recheck) {
  std::vector<InstalledMod> due;
  for (const auto &mod : installed) {
    if (mod.mod_id <= 0)
      continue;
    if (full_recheck || mod.last_checked <= 0) {
      due.push_back(mod);  // never checked: the cache cannot speak for it
      continue;
    }
    for (const auto &u : updated) {
      if (u.mod_id != mod.mod_id)
        continue;
      // Absent from the window, or untouched since we looked: it cannot have
      // changed, so no request is owed. A window entry with no timestamp (0)
      // counts as "moved" - the safe direction to be wrong in.
      if (u.latest_file_update == 0 || u.latest_file_update > mod.last_checked)
        due.push_back(mod);
      break;
    }
  }
  return due;
}

Period period_for(int64_t seconds_since_oldest_check) {
  // MO2 never requests a window narrower than the gap it has to cover, and a
  // wider window costs the same single request - so erring wide is free and
  // cannot miss an update. A month is the widest the endpoint offers, which
  // is why full_recheck_required() has to take over past that point.
  if (seconds_since_oldest_check <= 0)
    return Period::Month;  // never checked (or the clock went backwards)
  if (seconds_since_oldest_check >= kDay)
    return Period::Week;
  return Period::Day;
}

// ---------------------------------------------------------------------------
// run
// ---------------------------------------------------------------------------

Report run(const std::string &nexus_domain, const std::string &api_key,
           const std::filesystem::path &mods_dir, int64_t now) {
  Report report;
  if (nexus_domain.empty()) {
    report.message = "No Nexus source is registered for this game.";
    return report;
  }
  if (api_key.empty()) {
    report.message = "No Nexus API key configured.";
    return report;
  }

  int64_t oldest_checked = 0;
  const auto installed   = read_installed(mods_dir, oldest_checked);
  report.total           = static_cast<int>(installed.size());
  if (installed.empty()) {
    // Nothing of ours is on Nexus, so there is nothing to ask about: a
    // COMPLETED sweep that found nothing, and it cost zero requests. Report
    // ok=true so a caller cannot read this as a check that failed - that
    // would put an "update check failed" notice over an instance that simply
    // has no Nexus mods in it.
    report.ok = true;
    return report;
  }

  const int64_t gap = oldest_checked > 0 ? now - oldest_checked : 0;

  // --- The one bulk request this whole design rests on ---
  const auto reply =
      nexus_get("https://api.nexusmods.com/v1/games/" + nexus_domain +
                    "/mods/updated.json?period=" + period_query_value(period_for(gap)),
                api_key);
  ++report.requests;

  if (reply.refused) {
    report.rate_limited = true;
    report.message =
        "Nexus refused the update check: API request limit reached. Whether "
        "updates exist is unknown.";
    Logger::instance().warn("[NexusUpdateCheck] " + report.message);
    return report;
  }
  if (!reply.ok) {
    report.message = "Nexus update check failed (HTTP " +
                     std::to_string(reply.http_code) +
                     "). Whether updates exist is unknown.";
    Logger::instance().warn("[NexusUpdateCheck] " + report.message);
    return report;
  }

  std::vector<UpdatedMod> updated;
  if (!parse_updated(reply.body, updated)) {
    report.message =
        "Nexus returned an unreadable update list. Whether updates exist is "
        "unknown.";
    Logger::instance().warn("[NexusUpdateCheck] " + report.message);
    return report;
  }

  // --- Per-mod follow-ups, only for what the bulk call caught moving ---
  const bool full = full_recheck_required(gap);
  const auto due  = due_for_query(installed, updated, full);
  std::set<std::string> stamped;
  bool incomplete = false;

  for (const auto &mod : due) {
    const auto files =
        nexus_get("https://api.nexusmods.com/v1/games/" + nexus_domain + "/mods/" +
                      std::to_string(mod.mod_id) + "/files.json",
                  api_key);
    ++report.requests;
    if (!files.ok) {
      // Leave that mod's meta alone, lastnexusquery unbumped: the next sweep
      // retries it. Counting it as current would be the one lie that matters.
      incomplete = true;
      Logger::instance().warn("[NexusUpdateCheck] no file list for mod " +
                              std::to_string(mod.mod_id));
      continue;
    }

    const auto info = parse_files(files.body);
    ++report.checked;
    if (!info.ok) {
      incomplete = true;
      continue;
    }

    const auto state   = evaluate(mod.installed_file_id, info.available);
    const bool visible = update_visible(state.state == Verdict::Update,
                                        info.newest_version, mod.ignored_version);

    auto meta = ModMeta::load(mods_dir, mod.folder);
    meta.set(kSecNexus, kKeyLastQuery, utc_iso8601(now));
    meta.set(kSecGeneral, kKeyUpdate,
             state.state == Verdict::Update ? "true" : "false");
    if (!info.newest_version.empty())
      meta.set(kSecGeneral, kKeyNewest, info.newest_version);
    if (!meta.save(mods_dir, mod.folder)) {
      Logger::instance().warn("[NexusUpdateCheck] could not write meta.ini for " +
                              mod.folder);
      incomplete = true;
      continue;
    }
    stamped.insert(mod.folder);
    if (visible)
      ++report.updates;
  }

  // Everything the bulk window already answered for still gets re-stamped, so
  // the next sweep's window is computed from a truthful age and cannot outgrow
  // what a single request can cover.
  for (const auto &mod : installed) {
    if (stamped.count(mod.folder) == 0)
      stamp(mods_dir, mod.folder, now);
  }

  report.ok = true;
  if (incomplete) {
    report.message =
        "Update check finished, but some mods could not be reached and will be "
        "retried next time.";
  }
  return report;
}

// ---------------------------------------------------------------------------
// set_ignored_version
// ---------------------------------------------------------------------------

bool set_ignored_version(const std::filesystem::path &mods_dir,
                         const std::string &folder, bool ignore) {
  if (folder.empty())
    return false;
  auto meta = ModMeta::load(mods_dir, folder);
  if (ignore) {
    const std::string newest = read_newest_version(meta);
    if (newest.empty())
      return false;  // nothing to record; an empty ignore suppresses nothing
    meta.set(kSecGeneral, kKeyIgnored, newest);
  } else {
    meta.unset(kSecGeneral, kKeyIgnored);
  }
  return meta.save(mods_dir, folder);
}

}  // namespace engine::Source::Nexus::UpdateCheck