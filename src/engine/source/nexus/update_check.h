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

namespace engine {
class ModMeta;
}

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
  std::string ignored_version;      // [General] ignoredVersion
};

// Parse an updated.json body. Returns false (and leaves `out` untouched) for
// anything unparseable - which callers MUST report as a failed check, not as
// "nothing changed": an unread body and an empty list are indistinguishable
// downstream unless they are kept apart here.
bool parse_updated(const std::string &body, std::vector<UpdatedMod> &out);

// One mod's `files.json` answer: the ids Nexus still serves, the version
// string of the newest of them (which is what `newestversion` displays and
// what `ignoredVersion` has to match), and the version string of OUR OWN
// installed file.
//
// That last one is not decoration. `files[]` names every file the mod has ever
// had, each with the version string its author typed, so the version of the
// file we installed is already in this body - no second request, no guessing
// from a filename. It is also the authoritative installed version: MO2 stores
// exactly this string (downloadmanager.cpp:2274-2276 reads
// `file_details["version"]`, falling back to `mod_version` when it is absent)
// and reads it back out of meta.ini [General] version (modinforegular.cpp:94)
// to paint the Version column (modlist.cpp:198).
//
// The body is the captured Nexus v1 reply (tests/engine/fixtures/nexus/
// files_6832.json), and three names in it are the whole reason this function is
// exported:
//   - `files` is TOP LEVEL. There is no `data` envelope; `file_updates` is its
//     sibling. MO2 reads it off the bare document (mainwindow.cpp:3313-3315).
//   - `file_id` is the scalar id. `id` is also present but is the PAIR
//     `[file_id, mod_id]`, so it is not a number.
//   - there is no `available_mod_files` field.
// Getting any of them wrong yields an empty list, which downstream is
// indistinguishable from up to date. MO2 reads `file_id` from the same endpoint
// at mainwindow.cpp:3329, nexusinterface.cpp:132 and downloadmanager.cpp:1880.
//
// ARCHIVED entries are skipped, decided on `category_id` as MO2 decides it
// (nexusinterface.h:181-193). A retracted file is not downloadable, and counting
// it would let it masquerade as the newest one. `installed_version` stays empty
// when the installed id is absent from the list (retracted, or a meta.ini that
// names a file Nexus no longer serves) - an empty string there means "not
// stated", and callers must leave the recorded version alone.
bool parse_files(const std::string &body, long long installed_file_id,
                 std::vector<long long> &available_ids, std::string &newest_version,
                 std::string &installed_version);

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

// One mod the sweep could NOT answer for, and why. This exists because "no
// updates" and "could not check" must never look alike: the first is a result,
// the second is an absence of one, and a UI that renders both as a quiet
// "up to date" teaches the user the check works when it did not.
struct Unreachable {
  std::string folder;  // the mod folder under mods_dir
  std::string mod_id;  // its [Nexusmods] modid, for matching against a report
  std::string reason;  // why it could not be answered, in plain words
};

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

  // Every mod the sweep could not answer for, each with its reason. Empty on a
  // sweep that answered for everything it looked at. A mod listed here was NOT
  // stamped as checked, so the next sweep asks about it again.
  std::vector<Unreachable> unreachable;
};

// Sweep one game's installed mods and write the verdicts back to each mod's
// meta.ini. Synchronous and blocking (libcurl on the calling thread) - callers
// in the UI run it off the GUI thread.
//
// `nexus_domain` is the game's Nexus slug ("skyrimspecialedition");
// `mods_dir` is the instance's mods directory. Returns a Report whose ok flag
// distinguishes a completed sweep (even one that found nothing) from a sweep
// the API refused. Never throws.
//
// `force` ignores the `lastnexusquery` cache entirely and asks Nexus about
// every mod, as MO2's manual re-check does (ModInfo::manualUpdateCheck,
// modinfo.cpp:459-461 zeroes each mod's last-update time and then runs the
// ordinary sweep). Without it a stale stamp is permanent: a mod that was once
// stamped by a sweep which never got an answer keeps telling the next sweep it
// was just checked, so the update exists but nothing ever asks about it again.
// This costs one request per mod, which is why it is the explicit-action
// path and not the default.
//
// Anything it could not answer for is named in `unreachable` with the reason,
// in the log AND in the returned Report - never folded into a quiet zero.
Report run(const std::string &nexus_domain, const std::string &api_key,
           const std::filesystem::path &mods_dir, int64_t now, bool force = false);

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

// The ignored version already recorded in a mod's meta.ini, under whichever
// spelling that file carries.
//
// MO2's key is `ignoredVersion` (modinforegular.cpp:96 reads it, :260 writes
// it); this app used to write and read only the all-lowercase `ignoredversion`.
// The mismatch was silent in both directions - an ignore set in MO2 was dropped
// on import, and saving here left both spellings in one file. The lowercase
// form is still accepted on read so meta.ini files already on disk are not
// treated as "nothing ignored"; set_ignored_version() clears it once it writes,
// so a file never keeps a stale twin that would resurrect the ignore.
//
// Public because the mod list resolves the same key to paint the same badge:
// one reader, so the two cannot disagree about what the user ignored.
std::string read_ignored_version(const ModMeta &meta);

}  // namespace engine::Source::Nexus::UpdateCheck