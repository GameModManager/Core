#pragma once

// Nexus mod update check - "which of my installed mods has a newer version on
// Nexus?".
//
// The whole design exists because Nexus is metered: help.nexusmods.com/
// article/105 caps a key at 20000 requests per 24h (then 500/hour), and a key
// that keeps exceeding it gets banned. Asking Nexus once per installed mod
// would spend a fifth of the daily budget on a single check for a 200-mod
// library, and repeat that every day.
//
// So this asks ONCE per game per sweep:
//
//   GET /v1/games/{game}/mods/updated.json?period=1d|1w|1m
//
// which returns every mod on that game whose page changed inside the window,
// with the upstream timestamp. A mod of ours that is absent from that list,
// or present with a timestamp no newer than the moment we last looked at it,
// CANNOT have changed since - it needs no request at all. Only the mods the
// bulk call actually caught moving get a per-mod files.json call.
//
// That is MO2's mechanism verbatim: ModInfo::checkAllForUpdate (modinfo.cpp:
// 299-374) requests the bulk endpoint for the whole game, then per-mod
// queries only for what the bulk call flagged, and ModInfo::filteredMods
// (modinfo.cpp:389-434) stamps the untouched mods as checked without asking.
//
// The pure half (parsing, the installed-vs-available comparison, which mods a
// sweep still owes a request) is separated from the HTTP half so the whole of
// the decision-making is testable with fixture tables and no network.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace engine::Source::Nexus::UpdateCheck {

// The window asked of updated.json. A wider window costs exactly the same
// single request as a narrow one, so the ladder only ever trades freshness
// against the per-mod calls a stale cache would otherwise force.
enum class Period { Day, Week, Month };

// The `period=` query value. "1d" / "1w" / "1m", as MO2 sends them
// (NexusInterface::requestInfo, nexusinterface.cpp:906-919).
const char *period_query_value(Period p);

// One row of the updated.json response.
struct UpdatedMod {
  long long mod_id             = 0;
  long long latest_file_update = 0;  // Unix seconds; 0 when Nexus omitted it
};

// One of OUR mods, as far as this sweep is concerned.
struct InstalledMod {
  std::string folder;               // the folder under mods_dir; what meta.ini is named
  long long mod_id            = 0;  // [Nexusmods] modid
  long long installed_file_id = 0;  // [Nexusmods] fileid
  int64_t last_checked        = 0;  // [Nexusmods] lastnexusquery, Unix seconds;
                                    // 0 means never checked
  std::string newest_version;       // [General] newestversion (or MO2's casing)
  std::string ignored_version;      // [General] ignoredversion
};

// Parse an updated.json body. Returns false (and leaves `out` untouched) for
// anything unparseable - which callers MUST report as a failed check, not as
// "nothing changed": an unread body and an empty list are indistinguishable
// downstream unless they are kept apart here.
bool parse_updated(const std::string &body, std::vector<UpdatedMod> &out);

// What one installed mod's own file id says about the files Nexus still serves.
//
// Nexus file ids increase with upload order - the same assumption
// Provider::select_file_id already relies on (provider.cpp:67-69) - so the
// highest id still on offer is the newest file the mod has. That is the
// comparison: no version-string parsing, and it stays correct for the many
// Nexus mods whose version field reads "Update 3" or "1.0 - Fixed".
enum class Verdict {
  Current,  // our file is the newest Nexus still serves
  Update,   // a newer file exists
  Unknown,  // we could not tell (no installed id, or no available files)
};

struct UpdateState {
  Verdict state            = Verdict::Unknown;
  long long newest_file_id = 0;
};

// Compare our installed file against the ids Nexus still serves.
// `installed_file_id <= 0` (a mod we know nothing about) or an empty list
// (we could not read the file list) yields Unknown - never Update.
UpdateState evaluate(long long installed_file_id,
                     const std::vector<long long> &available_file_ids);

// MO2 ModInfoRegular::ignoreUpdate / ::updateAvailable (modinforegular.cpp:
// 330-338, :665-672): the ignore records the version the user turned away
// from, and suppresses the notice only while the newest version is STILL that
// one. A genuinely newer version shows up again. A stale ignore is never
// cleared here - only "Stop ignoring this version" removes it, as in MO2.
bool update_visible(bool available, std::string_view newest_version,
                    std::string_view ignored_version);

// True when the bulk window cannot answer for the cache we hold: the oldest
// check is older than the widest window the endpoint offers (a month), so
// every mod has to be asked about itself. Without this the gap between the
// last check and today would outrun the window and silently skip updates.
// MO2 does the same and warns that it costs significantly more
// (ModInfo::checkAllForUpdate, modinfo.cpp:341-364).
bool full_recheck_required(int64_t seconds_since_oldest_check);

// The mods a sweep still owes a per-mod request: those whose Nexus page has
// been touched since we last looked at them, or all of them when
// `full_recheck` says the window cannot speak for the cache. Everything else
// is answered from the cache and re-stamped - which is what keeps a settled
// library costing one request a sweep instead of one per mod.
//
// `full_recheck` is the answer to full_recheck_required(gap) for this sweep.
std::vector<InstalledMod> due_for_query(const std::vector<InstalledMod> &installed,
                                        const std::vector<UpdatedMod> &updated,
                                        bool full_recheck);

// How wide a window this sweep needs, from how long ago the OLDEST cached
// check was (`seconds_since_oldest_check`; 0 = never checked). MO2's ladder
// (ModInfo::checkAllForUpdate, modinfo.cpp:339-374): it never asks for less
// than the gap it has to cover, and a wider window costs the same one request.
Period period_for(int64_t seconds_since_oldest_check);

// ---------------------------------------------------------------------------
// Live sweep
// ---------------------------------------------------------------------------

// The outcome of one sweep, in the terms the UI has to report it in. `ok` and
// `message` travel together on purpose: an API refusal must never be
// renderable as "no updates available".
struct Report {
  bool ok             = false;
  bool rate_limited   = false;  // Nexus answered 429 / 403: out of budget
  std::string message = {};     // empty only on a sweep that fully succeeded
  int requests        = 0;      // API calls this sweep cost
  int updates         = 0;      // mods an update is being shown for
  int checked         = 0;      // mods whose file list we actually queried
  int total           = 0;      // Nexus-sourced mods in the instance
};

// Sweep one game's installed mods and write the verdicts back to each mod's
// meta.ini. Synchronous and blocking (libcurl on the calling thread) - callers
// in the UI run it off the GUI thread.
//
// `nexus_domain` is the game's Nexus slug ("skyrimspecialedition");
// `mods_dir` is the instance's mods directory. Returns a Report whose ok flag
// distinguishes a completed sweep (even one that found nothing) from a sweep
// the API refused. Never throws.
Report run(const std::string &nexus_domain, const std::string &api_key,
           const std::filesystem::path &mods_dir, int64_t now);

// Set or clear the version the user asked not to be told about, in one mod's
// own meta.ini. `newest_version` is recorded as the ignore when `ignore` is
// true, so a later sweep that publishes something genuinely newer shows up
// again. Per-mod, never per-profile: the thing being kept is a VERSION OF A
// MOD, which is a property of the mod folder and not of a load order.
//
// Fails (returns false, writes nothing) on a mod with no known newest
// version: there would be no version to record, and an empty ignore would
// suppress nothing.
bool set_ignored_version(const std::filesystem::path &mods_dir,
                         const std::string &folder, bool ignore);

}  // namespace engine::Source::Nexus::UpdateCheck