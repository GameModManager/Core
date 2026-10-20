#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace engine {

struct Mod;
struct PipelineContext;

}  // namespace engine

namespace engine::Source {

// Info a provider can resolve for a mod before download.
struct SourceDownloadInfo {
  std::string archive_name;  // real archive filename incl. extension (empty
                             // = keep the default "<source_id>[-<file_id>].zip")
  std::string display_name;  // human-readable mod/file name for the UI
};

// A source's request budget, formatted for a status-bar readout.
//
// `metered` is false for a source whose request rate we do not meter at all -
// a source with no API and no cooldown we enforce (LoversLab). The UI shows no
// readout for those, rather than a permanent "--" that advertises a number we
// never compute. A metered source fills `readout` with a real value on every
// call; each provider formats its own units, because they differ (Nexus has a
// server-side hourly+daily budget, Steam a client-side per-hour cooldown).
struct SourceRateLimit {
  bool        metered = false;
  std::string readout;
};

// What a downloads-panel row can tell a source about itself when the row's
// own identity never resolved, and what the source made of it. The Query
// Info action (MO2 downloadlistview.cpp:235) is offered per row only when
// the row's source answers can_query_info() for what the row carries, and
// the lookup itself runs through query_info() - the panel never talks to a
// source's own client directly.
struct DownloadInfoQuery {
  std::string page_url;      // stored source page URL (LoversLab / ModPub)
  std::string md5_hex;       // archive contents hash (Nexus md5_search)
  std::string archive_name;  // local archive filename (Nexus tie-breaker)
  std::string domain;        // game-domain hint (Nexus; empty for a Manual row
                             // the controller fills from the current game)
};

struct DownloadInfoResult {
  // Unsupported means "this source cannot answer for what this row carries"
  // - a result, not a failure; the panel says so and writes nothing.
  enum class Outcome {
    Unsupported,
    Matched,
    NotFound,
    Ambiguous,    // several candidates, no tie-breaker: write NOTHING
    Unreachable,  // the question could not be asked (transport, budget)
  };
  Outcome outcome = Outcome::Unsupported;
  std::string name;       // display name the source answered
  std::string version;    // version string the source answered
  long long mod_id  = 0;  // Nexus mod id to write back (0 = none)
  long long file_id = 0;  // Nexus file id to write back (0 = none)
  std::string domain;     // Nexus domain to write back
};

// Everything one source's update sweep needs, copied off the UI thread at
// request time - the same snapshot rule the Nexus sweep's request obeys: the
// worker must not reach the keyring, and the game's provider domain is
// UI-layer knowledge. Fields a source does not use stay empty.
struct UpdateSweepContext {
  std::filesystem::path mods_dir;  // instance mods_dir_path()
  std::string game_domain;         // game-domain hint (the game's Nexus slug);
                                   // empty for a game with no Nexus identity
  std::string credential;          // the source's stored credential, read on
                                   // the GUI thread (Nexus API key); never
                                   // logged
  int64_t now = 0;                 // Unix seconds
  bool force = false;              // ignore the cached check stamps
};

// The outcome of one source's update sweep, in the terms the UI has to report
// it in. `ok` and `message` travel together on purpose: a refused check must
// never be renderable as "no updates available". Per-mod verdicts do NOT
// travel here - each source writes them into its mods' own meta.ini, the way
// the Nexus sweep already does, and the UI re-reads the metas once.
struct SourceUpdateReport {
  bool ok = false;
  std::string message = {};  // empty only on a sweep that fully succeeded
  int updates = 0;           // mods an update is being shown for
  int total = 0;             // this source's mods in the instance
};

class Interface {
public:
  virtual ~Interface()                    = default;
  virtual std::string source_type() const = 0;
  // Download a mod to dest_path. Returns true on success. ctx is mutable so
  // providers can signal a pause (PipelineContext::download_paused) and read
  // the resume offset / abort flag.
  virtual bool fetch(const ::engine::Mod &mod, ::engine::PipelineContext &ctx,
                     const std::filesystem::path &dest_path) = 0;
  // Optional: resolve metadata about a mod before download (real archive
  // name, display name). Called by FetchStage on the worker thread before
  // fetch(). An empty struct means "no info available".
  virtual SourceDownloadInfo resolve_download_info(const ::engine::Mod &mod) const {
    return {};
  }

  // Human-readable provider name for the UI (Sources tab).
  virtual std::string display_name() const { return source_type(); }

  // Optional request-budget meter for the status bar. Default: not metered.
  virtual SourceRateLimit rate_limit_readout() const { return {}; }

  // Can this source identify a download row whose identity never resolved?
  // `query` carries what the row knows (page URL, game domain). A source
  // with no reliable match path for what the row carries returns false and
  // the panel hides the action - it never guesses.
  virtual bool can_query_info(const DownloadInfoQuery &query) const {
    (void)query;
    return false;
  }

  // Identify a download row. An ambiguous answer writes nothing - the same
  // no-guess rule every source obeys. The default cannot answer at all.
  virtual DownloadInfoResult query_info(const DownloadInfoQuery &query) const {
    (void)query;
    return {};
  }

  // Can this source check the installed mods under `ctx.mods_dir` for
  // updates? A source with no reliable update path for what it installed
  // returns false and the sweep skips it - it never guesses, and a mod of
  // that source simply gets no verdict and no decoration.
  virtual bool can_check_updates() const { return false; }

  // Sweep this source's installed mods and write each verdict into the mod's
  // own meta.ini, the way the Nexus sweep already does. Synchronous and
  // blocking - callers in the UI run it off the GUI thread. The default
  // cannot answer at all.
  virtual SourceUpdateReport check_updates(const UpdateSweepContext &ctx) const {
    (void)ctx;
    return {};
  }
};

}  // namespace engine::Source
