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
  // MO2's exact spelling (modinforegular.cpp:96 reads it, :260 writes it), so an
  // instance imported from MO2 keeps its ignore instead of silently losing it,
  // and an ignore set here is one MO2 still honours. The lowercase spelling is
  // what this app used to call the same key; kept only so meta.ini files that
  // still carry it are read rather than treated as "nothing ignored".
  constexpr const char *kKeyIgnored       = "ignoredVersion";
  constexpr const char *kKeyIgnoredLegacy = "ignoredversion";
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

  // Why one request could not be used, in words a user can act on. Every
  // failure path in the sweep funnels through here, so no mod can fail without
  // a name and a reason attached to it.
  std::string describe(const HttpReply &r) {
    if (r.refused)
      return "Nexus refused the request (API budget exhausted)";
    if (r.http_code == 0)
      return "the request never reached Nexus";
    return "Nexus answered HTTP " + std::to_string(r.http_code);
  }

  // Read an optional string member, tolerating the JSON `null` that Nexus uses
  // for "no value" - most notably `category_name` on a mod's main file, which
  // is most of them. json::value() THROWS on a null it is asked to convert, and
  // that throw aborts the parse for the whole mod, which is exactly the
  // "every mod comes back unreadable" outcome this parser already had once.
  std::string string_field(const nlohmann::json &object, const char *key) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string())
      return {};
    return it->get<std::string>();
  }

  // True for a file Nexus has retracted. An archived file is not downloadable,
  // so counting it would let a withdrawn file masquerade as the newest one.
  bool is_archived(const nlohmann::json &file) {
    return string_field(file, "category_name") == "ARCHIVED";
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

  // Same shape, same reason, for the ignored version - but that one is NOT
  // private: the mod list has to resolve the identical key to paint the same
  // badge, and an inline copy of this fallback there is a third place to fix
  // the next time the spelling is wrong. Defined below, in the namespace.

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
      mod.ignored_version   = read_ignored_version(meta);
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

std::string read_ignored_version(const ModMeta &meta) {
  // MO2's spelling first. It is the one it reads (modinforegular.cpp:96) and the
  // one this app now writes, so a file carrying both resolves to the value
  // being maintained rather than to a stale one left by an older build.
  std::string v = meta.get(kSecGeneral, kKeyIgnored);
  if (v.empty())
    v = meta.get(kSecGeneral, kKeyIgnoredLegacy);
  return v;
}

bool parse_files(const std::string &body, long long installed_file_id,
                 std::vector<long long> &available_ids, std::string &newest_version,
                 std::string &installed_version) {
  std::vector<long long> available;
  std::string newest;
  std::string installed;
  long long newest_id = 0;
  try {
    const auto j = nlohmann::json::parse(body);
    if (!j.contains("data") || !j["data"].is_object())
      return false;
    const auto &data = j["data"];
    if (!data.contains("files") || !data["files"].is_array())
      return false;

    // `file_id` is the name Nexus uses, in every one of its clients' code and
    // in MO2's (mainwindow.cpp:3329, nexusinterface.cpp:132). Reading `id`
    // here instead - or waiting for an `available_mod_files` field that does
    // not exist - silently yields an empty list, and an empty list means
    // "could not tell", so every mod in the instance would come back
    // undecided and no badge would ever light up.
    for (const auto &f : data["files"]) {
      if (!f.is_object())
        continue;
      const long long id = f.value("file_id", 0LL);
      if (id <= 0 || is_archived(f))
        continue;
      available.push_back(id);
      const std::string version = string_field(f, "version");
      if (id > newest_id) {
        newest_id = id;
        newest    = version;
      }
      // Our own file's version, from the same body. The Version column reads
      // [General] version, and an install that had no version to record wrote
      // a fabricated one there - so this is where the real string comes back
      // in. Empty when Nexus no longer lists the file we installed, which is
      // "not stated" and leaves the recorded value alone.
      if (id == installed_file_id)
        installed = version;
    }
  } catch (const std::exception &e) {
    Logger::instance().error(
        std::string("[NexusUpdateCheck] file list parse failed: ") + e.what());
    return false;
  }

  if (available.empty())
    return false;
  available_ids     = std::move(available);
  newest_version    = std::move(newest);
  installed_version = std::move(installed);
  return true;
}

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
           const std::filesystem::path &mods_dir, int64_t now, bool force) {
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
  // `force` is the user's explicit "ask Nexus now": it overrides the cache the
  // window is computed from, because a stamp left by a sweep that never got an
  // answer is indistinguishable from a real one on disk and would otherwise
  // suppress this mod forever.
  const bool full = force || full_recheck_required(gap);
  const auto due  = due_for_query(installed, updated, full);
  std::set<std::string> stamped;
  std::set<std::string> unresolved;

  // Every way a mod can fail to get an answer, in one place: the mod is named,
  // the reason is stated, and the reason reaches both the log and the Report.
  // A sweep that silently drops a mod is indistinguishable from one that found
  // it current, and that is the single failure this feature cannot have.
  const auto unanswerable = [&](const InstalledMod &mod, const std::string &reason) {
    report.unreachable.push_back({mod.folder, std::to_string(mod.mod_id), reason});
    unresolved.insert(mod.folder);
    Logger::instance().warn("[NexusUpdateCheck] could not check mod " + mod.folder +
                            " (nexus mod id " + std::to_string(mod.mod_id) +
                            "): " + reason);
  };

  for (const auto &mod : due) {
    const auto files =
        nexus_get("https://api.nexusmods.com/v1/games/" + nexus_domain + "/mods/" +
                      std::to_string(mod.mod_id) + "/files.json",
                  api_key);
    ++report.requests;
    if (!files.ok) {
      // Leave that mod's meta alone, lastnexusquery unbumped: the next sweep
      // retries it. Counting it as current would be the one lie that matters.
      if (files.refused)
        report.rate_limited = true;
      unanswerable(mod, describe(files));
      continue;
    }

    std::vector<long long> available;
    std::string newest_version;
    std::string installed_version;
    ++report.checked;
    if (!parse_files(files.body, mod.installed_file_id, available, newest_version,
                     installed_version)) {
      unanswerable(mod, "Nexus sent a file list this build cannot read");
      continue;
    }

    const auto state = evaluate(mod.installed_file_id, available);
    // parse_files only returns true with a non-empty list, so Unknown here means
    // exactly one thing: our own meta.ini records no Nexus file id, so there is
    // nothing to compare the list against. Recording nexus_update=false would
    // state as fact something nobody checked - the same lie as stamping the mod
    // as freshly checked, and the reason this mod is named instead.
    if (state.state == Verdict::Unknown) {
      unanswerable(mod,
                   "this mod's meta.ini records no Nexus file id to compare against");
      continue;
    }
    const bool visible = update_visible(state.state == Verdict::Update, newest_version,
                                        mod.ignored_version);

    auto meta = ModMeta::load(mods_dir, mod.folder);
    meta.set(kSecNexus, kKeyLastQuery, utc_iso8601(now));
    meta.set(kSecGeneral, kKeyUpdate,
             state.state == Verdict::Update ? "true" : "false");
    if (!newest_version.empty())
      meta.set(kSecGeneral, kKeyNewest, newest_version);
    // The INSTALLED version, from the same response. The Version column reads
    // [General] version (mod_scanner.cpp:560), and an install that had no
    // version to record put a fabricated "1.0" there
    // (ModMeta::write_game_metadata, mod_meta.cpp) - so every MO2-style game,
    // whose archives carry no XML version manifest, displayed 1.0 for every
    // mod. Nexus names the version of the exact file we installed, which is
    // what MO2 stores (downloadmanager.cpp:2274-2276) and reads back
    // (modinforegular.cpp:94), so this is the authoritative value arriving at
    // no extra cost. Only written when Nexus stated one: an absent version
    // means "unknown", and overwriting a good recorded value with nothing
    // would trade one wrong display for another.
    if (!installed_version.empty())
      meta.set(kSecGeneral, "version", installed_version);
    if (!meta.save(mods_dir, mod.folder)) {
      unanswerable(mod, "the verdict could not be written to its meta.ini");
      continue;
    }
    stamped.insert(mod.folder);
    if (visible)
      ++report.updates;
  }

  // Everything the bulk window already answered for still gets re-stamped, so
  // the next sweep's window is computed from a truthful age and cannot outgrow
  // what a single request can cover. A mod we could NOT answer for is excluded:
  // stamping it would record a check that never happened, and the next sweep
  // would then skip it as recently looked-at - turning a transient outage into
  // a permanent silent miss.
  for (const auto &mod : installed) {
    if (stamped.count(mod.folder) == 0 && unresolved.count(mod.folder) == 0)
      stamp(mods_dir, mod.folder, now);
  }

  report.ok = true;
  if (!report.unreachable.empty()) {
    report.message =
        std::to_string(report.unreachable.size()) +
        (report.unreachable.size() == 1
             ? " mod could not be reached on Nexus and will be retried: "
             : " mods could not be reached on Nexus and will be retried: ") +
        report.unreachable.front().folder + " (" + report.unreachable.front().reason +
        ")";
    if (report.unreachable.size() > 1)
      report.message +=
          ", and " + std::to_string(report.unreachable.size() - 1) + " more";
    Logger::instance().warn("[NexusUpdateCheck] " + report.message);
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
  // The legacy spelling is cleared on BOTH paths, not just the un-ignore one.
  // A file carrying both would otherwise keep answering "ignored" from the
  // stale twin after the user turned it off, which is worse than never having
  // honoured it. unset() is a no-op when the key is absent, so this costs
  // nothing on a file that has only ever used one spelling.
  meta.unset(kSecGeneral, kKeyIgnoredLegacy);
  return meta.save(mods_dir, folder);
}

}  // namespace engine::Source::Nexus::UpdateCheck