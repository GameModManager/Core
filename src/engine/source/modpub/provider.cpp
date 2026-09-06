#include "engine/source/modpub/provider.h"

#include "engine/core/log/logger.h"
#include "engine/mod/model/mod.h"
#include "engine/network/network_manager.h"
#include "engine/pipeline/pipeline.h"
#include "engine/source/http_util.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <regex>
#include <string>

namespace engine::Source::ModPub {

namespace {

// Cap on the response body we'll buffer. mod.pub mod pages are well under
// 100 KB; anything past a few MB is either a misconfigured server or
// hostile. Once we hit the cap we abort the transfer (returning 0 from the
// libcurl write callback is documented as the way to signal an abort).
constexpr size_t kMaxBodyBytes = 10 * 1024 * 1024;

// Lowercase ASCII. Avoids pulling in <algorithm> for a one-shot helper.
std::string to_lower_ascii(const std::string &in) {
  std::string out(in);
  for (auto &c : out)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

// Trim surrounding ASCII whitespace (used when pulling strings out of HTML
// attributes and JSON-LD text fields).
std::string trim(const std::string &s) {
  const auto first = s.find_first_not_of(" \t\r\n");
  if (first == std::string::npos)
    return {};
  const auto last = s.find_last_not_of(" \t\r\n");
  return s.substr(first, last - first + 1);
}

// Pull the value out of an HTML attribute like:
//   name="..."  or  name='...'
// Matches og:* tags too: og:title sits in the `property` attribute on a
// <meta> element, so the caller passes the property value (og:title) as
// `attr` and the search is constrained to <meta> tags. Attribute values
// are HTML-encoded, so decode the most common entities so the resulting
// string does not carry "&quot;" etc. Only used as a last-resort fallback
// when JSON-LD is missing or malformed.
std::string read_meta(const std::string &html, const std::string &attr) {
  // Build a tolerant regex that matches any attribute order. Real HTML
  // pages emit either:
  //   <meta property="og:..." content="...">
  //   <meta content="..." property="og:...">
  // and the same two with name= instead of property=. We allow any
  // whitespace between attributes and accept both orderings via two
  // alternatives joined with `|`. The capture group is the content= value.
  // The regex is reconstructed on each call (mod.pub emits 4-5 og:*
  // tags per page; LoversLab follows the same pattern). A static
  // std::regex would need a placeholder+replace dance that std::regex
  // does not support natively.
  const std::string key = "(?:property|name)";
  const std::regex kMeta("<meta\\s+(?:"
                             // property/name first, content second
                             + key + "=[\"']" + attr +
                             "[\"']\\s+content=[\"']([^\"']*)[\"']"
                             "|" +
                             // content first, property/name second
                             "content=[\"']([^\"']*)[\"']\\s+" + key +
                             "=[\"']" + attr +
                             "[\"']"
                             ")",
                         std::regex::icase);
  std::smatch m;
  if (!std::regex_search(html, m, kMeta))
    return {};
  std::string raw = m[1].matched ? m[1].str() : m[2].str();
  auto replace_all = [](std::string &s, const std::string &from,
                        const std::string &to) {
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
      s.replace(pos, from.size(), to);
      pos += to.size();
    }
  };
  replace_all(raw, "&amp;", "&");
  replace_all(raw, "&quot;", "\"");
  replace_all(raw, "&#39;", "'");
  replace_all(raw, "&lt;", "<");
  replace_all(raw, "&gt;", ">");
  replace_all(raw, "&nbsp;", " ");
  return trim(raw);
}

// Pull a string value out of the JSON object by key. Returns empty when the
// key is missing or the value is not a string.
std::string j_str(const nlohmann::json &j, const char *key) {
  if (!j.is_object() || !j.contains(key))
    return {};
  const auto &v = j.at(key);
  if (v.is_string())
    return trim(v.get<std::string>());
  return {};
}

// Build the canonical mod page URL from a (game_slug, mod_id) pair,
// always the bare "https://mod.pub/<slug>/<id>/" form. mod.pub's URL
// shape is "<game>/<id>-<slug-suffix>"; the suffix is reconstructible
// only by re-fetching the mod list, so we synthesize the bare form
// (no suffix) and let the parser's fallback overwrite page_url with
// the JSON-LD `url` field on success.
std::string build_page_url(const std::string &game_slug,
                           const std::string &mod_id) {
  return "https://mod.pub/" + game_slug + "/" + mod_id + "/";
}

// Pull the page's <aside> "category" tag out of the DOM. mod.pub tags each
// mod with a category (e.g. "User interface") that is NOT carried in the
// JSON-LD applicationCategory field (which is always "GameMod"). The
// aside sits next to the upload metadata; we match the visible text node
// after the label.
//
// The text is HTML-decoded by the same entity replacement read_meta() uses
// (via trim + a small entity pass); we only decode the entities that
// actually appear in mod.pub pages, no full spec compliance.
std::string read_aside_tag(const std::string &html) {
  // The aside line on the mod page looks like (whitespace variable):
  //   <aside>...<b>Tag</b> User interface ...</aside>
  // or in a sibling <div> with a class. We accept the text between the
  // label and the next tag/element boundary. The regex is intentionally
  // loose; the parser downstream trims the result.
  static const std::regex kTag(R"(<aside[^>]*>[\s\S]*?<b>\s*Tag\s*</b>\s*([^<]+))",
                               std::regex::icase);
  std::smatch m;
  if (!std::regex_search(html, m, kTag))
    return {};
  std::string raw = m[1].str();
  auto replace_all = [](std::string &s, const std::string &from,
                        const std::string &to) {
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
      s.replace(pos, from.size(), to);
      pos += to.size();
    }
  };
  replace_all(raw, "&amp;", "&");
  replace_all(raw, "&quot;", "\"");
  replace_all(raw, "&#39;", "'");
  replace_all(raw, "&nbsp;", " ");
  return trim(raw);
}

// Locate the JSON-LD block whose @type is "SoftwareApplication" (the
// schema mod.pub emits for mod pages). Returns the JSON text, or empty
// when none is found. The regex is intentionally simple: anything tagged
// application/ld+json whose body parses to an object with
// @type=SoftwareApplication wins. We never evaluate HTML or scripts.
std::string find_softwareapp_json_ld(const std::string &html) {
  static const std::regex kScript(
      R"(<script[^>]*type=["']application/ld\+json["'][^>]*>([\s\S]*?)</script>)",
      std::regex::icase);
  auto begin = std::sregex_iterator(html.begin(), html.end(), kScript);
  auto end = std::sregex_iterator();
  for (auto it = begin; it != end; ++it) {
    std::string body = trim((*it)[1].str());
    if (body.empty())
      continue;
    try {
      auto j = nlohmann::json::parse(body);
      auto check = [](const nlohmann::json &node) -> bool {
        if (!node.is_object())
          return false;
        auto t = j_str(node, "@type");
        if (t.empty())
          return false;
        return to_lower_ascii(t) == "softwareapplication";
      };
      if (check(j))
        return body;
      if (j.is_object() && j.contains("@graph") && j["@graph"].is_array()) {
        for (const auto &node : j["@graph"]) {
          if (check(node))
            return node.dump();
        }
      }
    } catch (const std::exception &) {
      continue;
    }
  }
  return {};
}

} // namespace

bool Provider::is_modpub_url(const std::string &url) {
  if (url.empty())
    return false;
  // Cheap pre-check before the more expensive host parse. The canonical
  // path always includes /<game>/<id>-<slug> or /<game>/<id>, so a URL
  // missing the second path segment is not a mod page.
  const std::string marker = "://mod.pub/";
  std::string low(url);
  for (auto &c : low)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (low.find(marker) == std::string::npos) {
    // Also accept bare "mod.pub/<slug>/<id>" pastes; the user might have
    // dropped the scheme. Match "mod.pub/" exactly.
    if (low.compare(0, 8, "mod.pub/") != 0)
      return false;
  }
  // After the host the path must contain at least one slash.
  const auto host_end = low.find("://");
  const std::size_t path_start =
      (host_end == std::string::npos) ? 8 : host_end + marker.size() - 1;
  return low.find('/', path_start) != std::string::npos;
}

std::string Provider::extract_game_slug(const std::string &url) {
  if (url.empty())
    return {};
  // Path is everything after the host. Accept either scheme://mod.pub/...
  // or bare mod.pub/... (the user-paste case).
  std::size_t start = 0;
  std::string low(url);
  for (auto &c : low)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  const auto scheme = low.find("://");
  if (scheme != std::string::npos) {
    if (low.compare(scheme + 3, 8, "mod.pub/") != 0)
      return {};
    start = scheme + 3 + 8;
  } else if (low.compare(0, 8, "mod.pub/") == 0) {
    start = 8;
  } else {
    return {};
  }
  const auto end = low.find_first_of("/?#", start);
  std::string slug = low.substr(
      start, (end == std::string::npos) ? std::string::npos : end - start);
  // Reject empty / pure-separator / non-slug inputs. The slug must be
  // kebab-case ASCII; a path like "mod.pub//" or "mod.pub/files/..."
  // should return empty (the latter is a different route, not a mod page).
  if (slug.empty() || slug.find('/') != std::string::npos)
    return {};
  for (const char c : slug) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    if (!ok)
      return {};
  }
  return slug;
}

std::string Provider::extract_mod_id(const std::string &url) {
  if (url.empty())
    return {};
  std::size_t start = 0;
  std::string low(url);
  for (auto &c : low)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  const auto scheme = low.find("://");
  if (scheme != std::string::npos) {
    if (low.compare(scheme + 3, 8, "mod.pub/") != 0)
      return {};
    start = scheme + 3 + 8;
  } else if (low.compare(0, 8, "mod.pub/") == 0) {
    start = 8;
  } else {
    return {};
  }
  // Skip the game-slug (everything up to the next '/'), then read the
  // numeric id up to the first non-digit (the slug suffix starts with
  // '-' or the path ends). Accept a trailing '/' on the URL.
  const auto slug_end = low.find('/', start);
  if (slug_end == std::string::npos)
    return {};
  std::size_t id_start = slug_end + 1;
  const std::size_t seg_end = low.find_first_of("/?&", id_start);
  std::string seg = low.substr(
      id_start,
      (seg_end == std::string::npos) ? std::string::npos : seg_end - id_start);
  // Drop the slug suffix after the first '-'.
  const auto dash = seg.find('-');
  std::string id = (dash == std::string::npos) ? seg : seg.substr(0, dash);
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
  std::string page = (cut == std::string::npos) ? url : url.substr(0, cut);
  // Always end with a trailing slash so the panel's "Open on ModPub"
  // builds a URL that matches what mod.pub itself renders.
  if (!page.empty() && page.back() != '/')
    page.push_back('/');
  return page;
}

ModInfoResult Provider::parse_mod_info(const std::string &html_body,
                                       const std::string &fallback_url) {
  ModInfoResult result;
  if (html_body.empty() && fallback_url.empty())
    return result;

  // --- Step 1: prefer the schema.org SoftwareApplication JSON-LD block.
  std::string ld_body = find_softwareapp_json_ld(html_body);
  if (!ld_body.empty()) {
    try {
      auto j = nlohmann::json::parse(ld_body);
      if (j.is_object()) {
        result.name = j_str(j, "name");
        result.version = j_str(j, "softwareVersion");
        // applicationCategory on mod.pub is the boilerplate "GameMod"
        // string. We use the aside tag (DOM-only) as a more useful
        // category below.
        result.category = j_str(j, "applicationCategory");
        result.description = j_str(j, "description");
        result.date_modified = j_str(j, "dateModified");
        if (j.contains("author")) {
          const auto &a = j.at("author");
          if (a.is_object())
            result.author = j_str(a, "name");
          else if (a.is_string())
            result.author = trim(a.get<std::string>());
        }
        auto url = j_str(j, "url");
        if (!url.empty())
          result.page_url = url;
      }
    } catch (const std::exception &) {
      // Malformed JSON-LD - drop everything and fall through to og:*.
      result = {};
    }
  }

  // --- Step 2: prefer the DOM aside tag for category when application-
  // Category is the boilerplate "GameMod". The mod.pub aside sits
  // adjacent to "Tag" and contains the human-readable tag.
  if (result.category.empty() || result.category == "GameMod") {
    const std::string tag = read_aside_tag(html_body);
    if (!tag.empty())
      result.category = tag;
  }

  // --- Step 3: fallback to og:* meta tags. Used when JSON-LD is absent
  // or did not produce a name. description and dateModified remain
  // empty when og:* does not carry them.
  if (result.name.empty())
    result.name = read_meta(html_body, "og:title");
  if (result.description.empty())
    result.description = read_meta(html_body, "og:description");
  if (result.author.empty())
    result.author = read_meta(html_body, "article:author");
  if (result.date_modified.empty())
    result.date_modified = read_meta(html_body, "og:updated_time");

  // Pull the game-slug out of the fallback URL when the JSON-LD url
  // didn't already carry it (the page sometimes advertises a different
  // URL via og:url).
  if (result.game_slug.empty() && !fallback_url.empty()) {
    result.game_slug = extract_game_slug(fallback_url);
  }

  // Pin page_url to the URL we fetched when JSON-LD did not advertise
  // one (or advertised a different one - the panel's "Open on" uses
  // the user's stored page_url).
  if (result.page_url.empty())
    result.page_url = fallback_url;

  // Available gate: the page must have advertised both a name and at
  // least a description. An empty result is indistinguishable from a
  // failure and should not be treated as success. mod.pub's NSFW / CF
  // redirects to a login page that has neither, so they correctly map
  // to available=false here.
  result.available = !result.name.empty() && !result.description.empty();
  return result;
}

ModInfoResult Provider::fetch_mod_info(const std::string &url_or_id) const {
  ModInfoResult result;
  if (url_or_id.empty())
    return result;

  std::string url;
  std::string game_slug;
  std::string mod_id;

  if (url_or_id.find('/') != std::string::npos) {
    // Looks like a path. Either a full URL or a "game/id" pair the user
    // typed in the AddSourceDialog. We accept both - is_modpub_url
    // gates the URL form.
    if (is_modpub_url(url_or_id)) {
      url = mod_page_url(url_or_id);
      if (url.empty())
        return result;
      // Normalize bare-host pastes ("mod.pub/skyrim-se/22-foo") by
      // prepending https:// - is_modpub_url accepts that form but
      // libcurl would reject it as a relative URL otherwise.
      if (url.compare(0, 7, "http://") != 0 &&
          url.compare(0, 8, "https://") != 0) {
        url = "https://" + url;
      }
      game_slug = extract_game_slug(url_or_id);
      // mod_id is recoverable from the URL but we do not need it here
      // - the URL is already the page the provider will GET. Kept for
      // symmetry with the "game/id" branch (where the id is parsed
      // back out of the second segment).
    } else {
      // "game/id" paste. Split on the slash.
      const auto slash = url_or_id.find('/');
      if (slash == std::string::npos || slash == 0 ||
          slash + 1 >= url_or_id.size())
        return result;
      game_slug = url_or_id.substr(0, slash);
      mod_id = url_or_id.substr(slash + 1);
      // mod_id may carry a "-slug" suffix; strip it.
      const auto dash = mod_id.find('-');
      if (dash != std::string::npos)
        mod_id.resize(dash);
      if (mod_id.empty())
        return result;
      for (const char c : mod_id) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
          mod_id.clear();
          break;
        }
      }
      if (mod_id.empty())
        return result;
      url = build_page_url(game_slug, mod_id);
    }
  } else {
    // Bare numeric id. mod.pub's URL shape is
    //   https://mod.pub/<game-slug>/<id>-<slug-suffix>
    // - the game-slug is part of the mod's identity and is NOT
    // reconstructible from the id alone. A bare-id paste therefore
    // cannot be resolved to a real mod page (the synthetic
    // https://mod.pub/mods/<id>/ 404s). The provider requires either
    // a full URL or a "game/id" pair; this branch exists only to keep
    // the API symmetric with LoversLab's bare-id acceptance, where
    // /files/file/<id>/ is canonical. We surface a debug log and bail
    // out so the panel can show "page URL required" via the
    // available=false gate.
    bool digits_only = !url_or_id.empty();
    for (const char c : url_or_id) {
      if (!std::isdigit(static_cast<unsigned char>(c))) {
        digits_only = false;
        break;
      }
    }
    if (!digits_only) {
      Logger::instance().debug(
          "ModPubProvider: fetch_mod_info id is not numeric");
      return result;
    }
    Logger::instance().debug(
        "ModPubProvider: fetch_mod_info got a bare numeric id - mod.pub "
        "requires a full URL or 'game/id' pair (game-slug is part of the "
        "mod identity and cannot be reconstructed from the id alone)");
    return result;
  }

  // Guest scrape. Metadata is guest-visible; we deliberately do NOT send
  // any session cookie here. Downloads require auth (modl://) and are
  // not routed through this provider.
  network::Request req;
  req.url = Http::encode_url_path(url);
  req.caller = NET_CALLER;
  req.timeout = std::chrono::seconds(15);
  req.follow_redirect = true;
  // Cap the response body. mod.pub mod pages are well under 100 KB; the
  // transfer aborts if it exceeds kMaxBodyBytes.
  req.max_bytes = static_cast<std::int64_t>(kMaxBodyBytes);
  req.headers.push_back(
      "User-Agent: GameModManager/0.1 (ModPub Provider)");

  auto resp = network::instance().request(req);
  if (!resp.error.empty()) {
    Logger::instance().debug(
        "ModPubProvider: fetch_mod_info curl error: " + resp.error);
    return result;
  }
  // NSFW mods and Cloudflare challenges redirect to /account/login (HTTP
  // 200) which carries no JSON-LD block. Non-200 statuses here mean a
  // genuine failure (offline / 5xx / 404 for a removed mod). The
  // available=false gate in parse_mod_info covers the redirect case.
  if (resp.http_code != 200) {
    Logger::instance().debug(
        "ModPubProvider: fetch_mod_info HTTP " +
        std::to_string(resp.http_code) +
        " - the mod may be removed or the site is rejecting the request");
    return result;
  }

  result = parse_mod_info(resp.body, url);
  // Pin page_url to the URL we fetched (parse_mod_info may have set it
  // from the JSON-LD `url` field, which usually agrees but is not
  // guaranteed).
  if (result.page_url.empty())
    result.page_url = url;
  // Backfill game_slug from the path when JSON-LD / og:url didn't carry
  // one. The panel needs it for the bare-URL Visit fallback.
  if (result.game_slug.empty())
    result.game_slug = game_slug;
  if (result.available) {
    Logger::instance().debug(
        "ModPubProvider: fetched mod info for " + url + " (date_modified: " +
        (result.date_modified.empty() ? "<none>" : result.date_modified) + ")");
  }
  return result;
}

bool Provider::fetch(const ::engine::Mod &mod, ::engine::PipelineContext &ctx,
                     const std::filesystem::path &dest_path) {
  (void)ctx;
  (void)dest_path;
  // ModPub is metadata-only. The download path for a ModPub mod is the
  // companion modl:// protocol: the user's browser hands the app a
  // modl://... URL via the OS handler, and the modl Provider does the
  // actual fetch. If the pipeline ever routes a "modpub"-sourced mod
  // through here (it should not), we refuse so the FetchStage aborts
  // cleanly rather than silently misbehaving.
  if (mod.download_source_type == "modpub") {
    Logger::instance().error(
        "ModPubProvider: fetch() called for a ModPub-sourced mod - "
        "ModPub has no public download API; use modl:// for downloads");
  }
  return false;
}

SourceDownloadInfo
Provider::resolve_download_info(const ::engine::Mod &mod) const {
  (void)mod;
  // No archive to resolve (the metadata-only design). The Downloads tab
  // synthesizes a placeholder from the page URL via the modl flow.
  return {};
}

std::string Provider::display_name() const { return "ModPub"; }

} // namespace engine::Source::ModPub
