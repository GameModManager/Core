#include "engine/source/loverslab/provider.h"
#include "engine/source/download/curl_download.h"
#include "engine/source/loverslab/auth.h"
#include "engine/mod/meta/mod_meta.h"
#include "engine/mod/model/mod.h"
#include "engine/network/network_manager.h"
#include "engine/pipeline/pipeline.h"
#include "engine/log/logger.h"
#include "engine/source/http_util.h"
#include "engine/source/nexus/update_check.h"
#include "engine/source/page_update.h"

#include <chrono>
#include <cctype>
#include <filesystem>
#include <string>
#include <thread>

namespace engine::Source::LoversLab {

namespace {

  struct Probe {
    std::string content_disposition;
    std::string effective_url;
    long http_code = 0;
  };

  // Header-only GET via Network:: - we want only the Content-Disposition
  // header (and the final URL after redirects), so we cap the body to 1 byte
  // to abort after the headers. Network:: applies redaction + logging
  // automatically; the cookie is sent as a regular Cookie header and never
  // reaches the log.
  Probe probe_download(const std::string &url, const std::string &cookie) {
    Probe p;
    network::Request req;
    req.url             = Http::encode_url_path(url);
    req.caller          = NET_CALLER;
    req.timeout         = std::chrono::seconds(30);
    req.follow_redirect = true;
    req.max_bytes       = 1;  // abort the body, headers already captured
    if (!cookie.empty())
      req.headers.push_back("Cookie: " + cookie);

    auto r                = network::instance().request(req);
    p.http_code           = r.http_code;
    p.effective_url       = r.effective_url;
    p.content_disposition = r.content_disposition;
    return p;
  }

  // "https://host/a/b/file.7z?x=1" -> "file.7z"
  std::string url_path_basename(const std::string &url) {
    const std::size_t scheme = url.find("://");
    const std::size_t start  = (scheme == std::string::npos) ? 0 : scheme + 3;
    const std::size_t slash  = url.find('/', start);
    if (slash == std::string::npos)
      return {};
    const std::size_t q = url.find_first_of("?#", slash);
    std::string path    = url.substr(
        slash + 1, (q == std::string::npos) ? std::string::npos : q - slash - 1);
    const std::size_t last = path.find_last_of('/');
    if (last != std::string::npos)
      path = path.substr(last + 1);
    return engine::download::percent_decode(path);
  }

  std::string to_lower(const std::string &in) {
    std::string out = in;
    for (auto &c : out)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
  }

}  // namespace

bool Provider::is_loverslab_url(const std::string &url) {
  std::size_t start     = url.find("://");
  start                 = (start == std::string::npos) ? 0 : start + 3;
  const std::size_t end = url.find_first_of("/?#", start);
  std::string host =
      url.substr(start, (end == std::string::npos) ? std::string::npos : end - start);
  const std::size_t colon = host.find(':');
  if (colon != std::string::npos)
    host = host.substr(0, colon);
  host = to_lower(host);

  if (host != "loverslab.com" &&
      !(host.size() > 14 && host.compare(host.size() - 14, 14, ".loverslab.com") == 0))
    return false;
  return url.find("/files/file/") != std::string::npos;
}

std::string Provider::extract_file_id(const std::string &url) {
  const std::string marker = "/files/file/";
  const std::size_t pos    = url.find(marker);
  if (pos == std::string::npos)
    return {};
  const std::size_t start   = pos + marker.size();
  const std::size_t seg_end = url.find_first_of("/?&", start);
  std::string seg = url.substr(start, (seg_end == std::string::npos) ? std::string::npos
                                                                     : seg_end - start);
  const std::size_t dash = seg.find('-');
  std::string id         = (dash == std::string::npos) ? seg : seg.substr(0, dash);
  if (id.empty())
    return {};
  for (const char c : id) {
    if (!std::isdigit(static_cast<unsigned char>(c)))
      return {};
  }
  return id;
}

std::string Provider::mod_page_url(const std::string &url) {
  const auto cut = url.find_first_of("?#");
  return url.substr(0, cut);
}

bool Provider::fetch(const Mod &mod, PipelineContext &ctx,
                     const std::filesystem::path &dest_path) {
  if (mod.download_source_type != "loverslab")
    return false;

  if (mod.download_url.empty()) {
    Logger::instance().error("LoversLabProvider: no download URL");
    return false;
  }

  const std::string cookie = Auth::instance().get_cookie();
  if (cookie.empty()) {
    Logger::instance().error("LoversLabProvider: no session cookie configured "
                             "(Settings > Sources > LoversLab)");
    return false;
  }

  engine::download::Progress dp;
  dp.callback     = ctx.on_progress;
  dp.should_abort = ctx.should_abort;
  dp.resume_base  = ctx.download_resume_from;
  dp.start        = std::chrono::steady_clock::now();

  engine::download::Options opts;
  opts.cookie_header = cookie;
  opts.user_agent    = "GameModManager/0.1 (LoversLab Provider)";
  opts.long_lived    = true;

  std::string effective_url;
  opts.effective_url = &effective_url;

  long http_code = 0;
  bool aborted   = false;
  if (!engine::download::curl_download(mod.download_url, dest_path, http_code, opts,
                                       &dp, ctx.download_resume_from, &aborted,
                                       NET_CALLER)) {
    if (aborted) {
      // Pause requested - partial file is kept for resume.
      ctx.download_paused = true;
      Logger::instance().debug(
          "LoversLabProvider: download aborted (pause), partial kept at " +
          dest_path.string());
    } else if (http_code == 401 || http_code == 403) {
      Logger::instance().error(
          "LoversLabProvider: download rejected (HTTP " + std::to_string(http_code) +
          ") - the session cookie is expired or does not match the link; "
          "re-copy it from a signed-in LoversLab tab");
    } else if (http_code == 0) {
      Logger::instance().error(
          "LoversLabProvider: download failed before any HTTP response "
          "(the session cookie must be 'name=value; name=value' pairs "
          "with no line breaks - re-save it under Settings > Sources > "
          "LoversLab)");
    } else {
      Logger::instance().error("LoversLabProvider: download failed (HTTP " +
                               std::to_string(http_code) + ")");
    }
    return false;
  }

  // The final URL after redirects can carry the csrfKey= token (a redirect
  // Location from the site keeps the signed query), so it goes through the
  // redactor like every other log of a signed URL.
  Logger::instance().debug("LoversLabProvider: download complete -> " +
                           dest_path.string() + " (final URL: " +
                           (effective_url.empty()
                                ? "<unknown>"
                                : network::redaction::redact_url(effective_url)) +
                           ")");
  return true;
}

SourceDownloadInfo Provider::resolve_download_info(const Mod &mod) const {
  SourceDownloadInfo info;
  if (mod.download_source_type != "loverslab" || mod.download_url.empty())
    return info;

  const std::string cookie = Auth::instance().get_cookie();
  if (cookie.empty())
    return info;

  const Probe p = probe_download(mod.download_url, cookie);
  if (p.http_code == 401 || p.http_code == 403) {
    Logger::instance().warn(
        "LoversLabProvider: download-link probe rejected (HTTP " +
        std::to_string(p.http_code) +
        ") - the session cookie is expired or does not match the link");
    return info;
  }
  if (p.http_code >= 400) {
    Logger::instance().debug("LoversLabProvider: download-link probe failed (HTTP " +
                             std::to_string(p.http_code) + "), using default names");
    return info;
  }

  std::string fname =
      engine::download::parse_content_disposition_filename(p.content_disposition);
  if (fname.empty())
    fname = url_path_basename(p.effective_url);
  if (fname.empty())
    return info;

  info.archive_name = fname;
  // Surface the archive basename (minus its extension) as the human-readable
  // name so the Downloads tab and the install folder get a real name instead
  // of the "LoversLab file <id>" placeholder. It rides the same
  // display_name -> mod.name -> download_complete rename chain the Nexus
  // provider uses; when nothing resolves here, info stays empty and the
  // placeholder remains as the last-resort fallback.
  info.display_name = std::filesystem::path(fname).stem().string();
  return info;
}

std::string Provider::display_name() const {
  return "LoversLab";
}

// ---------------------------------------------------------------------------
// Provider::query_info (Query Info, MO2 downloadlistview.cpp:235)
// ---------------------------------------------------------------------------

bool Provider::can_query_info(const DownloadInfoQuery &query) const {
  // The stored page URL is the match path: it names the exact file the
  // download came from, so the answer is provenance, not a guess. A row
  // without one (a legacy manifest entry) has nothing to match against -
  // hide rather than guess.
  return !query.page_url.empty();
}

DownloadInfoResult Provider::query_info(const DownloadInfoQuery &query) const {
  DownloadInfoResult result;
  const ModInfoResult info = fetch_mod_info(query.page_url);
  if (!info.available) {
    // A failed scrape (network error, HTTP != 200, login/NSFW gate, parse
    // failure) and a dead file id look the same here, so the honest answer
    // is "the question could not be asked" - never "no such mod".
    result.outcome = DownloadInfoResult::Outcome::Unreachable;
    return result;
  }
  result.outcome = DownloadInfoResult::Outcome::Matched;
  result.name    = info.name;
  result.version = info.version;
  return result;
}

// ---------------------------------------------------------------------------
// Provider::check_updates (the mod-update sweep)
// ---------------------------------------------------------------------------

bool Provider::can_check_updates() const { return true; }

SourceUpdateReport Provider::check_updates(const UpdateSweepContext &ctx) const {
  SourceUpdateReport report;
  std::error_code ec;
  if (!std::filesystem::is_directory(ctx.mods_dir, ec))
    return report;  // no mods dir: nothing to check, and the caller says so

  // The page state each verdict compares against, as the mod's meta.ini
  // holds it. Both newestversion spellings are read the way the Nexus sweep
  // reads them: the install stage writes newestVersion, the checks write
  // newestversion, and a badge that disagreed with the row's own version
  // column would be worse than no badge.
  const auto known_newest = [](const engine::ModMeta &meta) {
    std::string v = meta.get("General", "newestversion");
    if (v.empty())
      v = meta.get("General", "newestVersion");
    return v;
  };

  int unreachable = 0;
  std::string first_unreachable;  // "<folder> (<reason>)", for the report
  bool first_entry                = true;

  for (const auto &dir : std::filesystem::directory_iterator(ctx.mods_dir, ec)) {
    if (ec)
      break;
    if (!dir.is_directory())
      continue;
    const std::string folder = dir.path().filename().string();
    auto meta                = engine::ModMeta::load(ctx.mods_dir, folder);
    if (meta.source_type() != "loverslab")
      continue;

    // The page URL is the mod's provenance - the same match path Query Info
    // uses. A mod without one has nothing to scrape and gets no verdict at
    // all, the way Query Info hides; it is not counted as checked either.
    std::string page_url = meta.get("LoversLab", "page_url");
    if (page_url.empty()) {
      const std::string file_id = meta.get("LoversLab", "fileid");
      if (!file_id.empty())
        page_url = "https://www.loverslab.com/files/file/" + file_id + "/";
    }
    if (page_url.empty())
      continue;

    ++report.total;
    // LoversLab rate limits aggressively and has no API budget to read, so
    // the only pacing is ours: space the scrapes out rather than firing one
    // GET per mod back to back. The first scrape is immediate.
    if (!first_entry)
      std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    first_entry = false;

    const ModInfoResult info = fetch_mod_info(page_url);
    if (!info.available) {
      // A failed scrape writes NOTHING - the same rule the Nexus sweep
      // obeys: "could not check" must never be recorded as "up to date".
      ++unreachable;
      if (first_unreachable.empty())
        first_unreachable = folder + " (the page could not be fetched)";
      Logger::instance().warn(
          "[LoversLabUpdateCheck] could not check mod " + folder + ": the page " +
          network::redaction::redact_url(page_url) + " could not be fetched");
      continue;
    }

    const auto verdict =
        compare_page_update(info.version, info.date_modified, known_newest(meta),
                            meta.get("LoversLab", "date_modified"),
                            meta.get("General", "version"));

    // Record the page state so the NEXT check has a basis, whatever the
    // verdict. NoBasis writes no verdict flag: the mod shows no badge, and
    // nothing in meta.ini claims it was checked.
    if (!info.version.empty())
      meta.set("General", "newestversion", info.version);
    if (!info.date_modified.empty())
      meta.set("LoversLab", "date_modified", info.date_modified);
    if (verdict.outcome != PageUpdateOutcome::NoBasis) {
      meta.set("General", "update_available",
               verdict.outcome == PageUpdateOutcome::Update ? "true" : "false");
      // The report counts what the user will SEE: an update the user has
      // already ignored is stamped but not announced, exactly as the Nexus
      // sweep counts.
      const bool visible = Nexus::UpdateCheck::update_visible(
          verdict.outcome == PageUpdateOutcome::Update, verdict.newest_version,
          Nexus::UpdateCheck::read_ignored_version(meta));
      if (verdict.outcome == PageUpdateOutcome::Update && visible)
        ++report.updates;
    }
    if (!meta.save(ctx.mods_dir, folder)) {
      ++unreachable;
      if (first_unreachable.empty())
        first_unreachable = folder + " (the verdict could not be written to its meta.ini)";
    }
  }

  if (unreachable > 0) {
    report.message = std::to_string(unreachable) +
                     (unreachable == 1 ? " mod could not be reached on LoversLab"
                                       : " mods could not be reached on LoversLab") +
                     " and will be retried: " + first_unreachable;
    if (unreachable > 1)
      report.message += ", and " + std::to_string(unreachable - 1) + " more";
  }
  report.ok = true;
  return report;
}

}  // namespace engine::Source::LoversLab
