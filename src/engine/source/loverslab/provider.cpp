#include "engine/source/loverslab/provider.h"
#include "engine/core/log/logger.h"
#include "engine/network/network_manager.h"
#include "engine/source/http_util.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <regex>
#include <string>

namespace engine::Source::LoversLab {

namespace {

// Cap on the response body we'll buffer. LoversLab mod pages are well under
// 100 KB; anything past a few MB is either a misconfigured server or hostile.
// Once we hit the cap we abort the transfer (returning 0 from the libcurl
// write callback is documented as the way to signal an abort).
constexpr size_t kMaxBodyBytes = 10 * 1024 * 1024;

// libcurl write callback that accumulates the response body into a string.
// Aborts the transfer (returns 0) once we have buffered kMaxBodyBytes so a
// hostile or misconfigured server cannot push us into OOM.
size_t append_body(char *ptr, size_t size, size_t nmemb, void *userdata) {
  auto *out = static_cast<std::string *>(userdata);
  const size_t total = size * nmemb;
  if (out->size() + total > kMaxBodyBytes) {
    // Truncate to the cap and abort the transfer.
    if (out->size() < kMaxBodyBytes)
      out->append(ptr, kMaxBodyBytes - out->size());
    return 0; // signals libcurl to abort with CURLE_WRITE_ERROR
  }
  out->append(ptr, total);
  return total;
}

// Lowercase ASCII. std::transform over <ctype.h>'s tolower with the
// unsigned-char cast avoids the locale-dependent sign-extension trap.
std::string to_lower_ascii(const std::string &in) {
  std::string out(in);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
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
  std::string body = html;
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
  if (!std::regex_search(body, m, kMeta))
    return {};
  // Pick whichever capture group the match populated.
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

// Build the canonical mod-page URL from a file id (always the bare
// /files/file/{id}/ form so the link in the UI does not carry the slug -
// the page is the same either way, and the bare form survives URL paste
// round-trips cleanly).
std::string build_page_url(const std::string &file_id) {
  return "https://www.loverslab.com/files/file/" + file_id + "/";
}

// Locate the JSON-LD block whose @type is "WebApplication" (the schema
// Invision Community emits for mod pages). Returns the JSON text, or
// empty when none is found. The regex is intentionally simple: anything
// tagged application/ld+json whose body parses to an object with
// @type=WebApplication wins. We never evaluate HTML or scripts.
std::string find_webapp_json_ld(const std::string &html) {
  // Match <script type="application/ld+json"> ... </script>, non-greedy.
  // Use DOTALL-ish behavior with [\s\S]*?; the regex grammar here is
  // std::regex (ECMAScript), where . does not match newlines by default.
  static const std::regex kScript(
      R"(<script[^>]*type=["']application/ld\+json["'][^>]*>([\s\S]*?)</script>)",
      std::regex::icase);
  auto begin = std::sregex_iterator(html.begin(), html.end(), kScript);
  auto end = std::sregex_iterator();
  for (auto it = begin; it != end; ++it) {
    std::string body = (*it)[1].str();
    // Trim leading whitespace - some pages prefix a BOM or newline.
    body = trim(body);
    if (body.empty())
      continue;
    try {
      auto j = nlohmann::json::parse(body);
      // Walk either a single object or a @graph array.
      auto check = [](const nlohmann::json &node) -> bool {
        if (!node.is_object())
          return false;
        auto t = j_str(node, "@type");
        if (t.empty())
          return false;
        std::string tl = to_lower_ascii(t);
        return tl == "webapplication";
      };
      if (check(j))
        return body;
      if (j.is_object() && j.contains("@graph") && j["@graph"].is_array()) {
        const auto found = std::find_if(
            j["@graph"].begin(), j["@graph"].end(), check);
        if (found != j["@graph"].end())
          return found->dump();
      }
    } catch (const std::exception &) {
      // Malformed JSON-LD block - skip and try the next one.
      continue;
    }
  }
  return {};
}

// Walk a JSON-LD node's @graph (when present) looking for a WebApplication
// entry, then return that object as JSON text. Otherwise return the input
// string (so the caller can reuse its existing object parse).
std::string extract_webapp_object(const std::string &body) {
  try {
    auto j = nlohmann::json::parse(body);
    if (j.is_object() && j.contains("@graph") && j["@graph"].is_array()) {
      for (const auto &node : j["@graph"]) {
        if (!node.is_object())
          continue;
        auto t = j_str(node, "@type");
        if (!t.empty() && to_lower_ascii(t) == "webapplication")
          return node.dump();
      }
    }
    return body;
  } catch (const std::exception &) {
    return body;
  }
}

// Pure HTML-attribute allowlist for the rich-text description block.
// Mirrors the scheme list the BBCode layer uses (see
// ui/modinfo/bbcode.cpp::is_url_scheme_ok). javascript:, data:, etc.
// are dropped - the link text survives as plain text and the
// surrounding page is never handed an executable URL.
bool is_safe_description_url(const std::string &url) {
  if (url.empty())
    return false;
  auto starts_with_ci = [](const std::string &s, const char *prefix) {
    const size_t n = std::strlen(prefix);
    if (s.size() < n)
      return false;
    for (size_t i = 0; i < n; ++i) {
      char a = s[i];
      char b = prefix[i];
      if (a >= 'A' && a <= 'Z')
        a = static_cast<char>(a - 'A' + 'a');
      if (b >= 'A' && b <= 'Z')
        b = static_cast<char>(b - 'A' + 'a');
      if (a != b)
        return false;
    }
    return true;
  };
  return starts_with_ci(url, "http://") ||
         starts_with_ci(url, "https://") ||
         starts_with_ci(url, "mailto:") ||
         starts_with_ci(url, "ftp://") ||
         starts_with_ci(url, "ftps://");
}

// Convert a single HTML anchor tag into a BBCode [url=...]text[/url] or,
// when the href is unsafe, into the visible text alone. Operates on the
// first match in `html`; the caller iterates until no match remains.
// Returns true if a replacement was made.
bool convert_anchor_once(std::string &html) {
  // Two regexes: double-quoted href, then single-quoted. Both use a
  // non-greedy body so the first </a> closes the tag.
  static const std::regex kAnchorDq(
      "<a\\b[^>]*\\bhref\\s*=\\s*\"([^\"]*)\"[^>]*>([\\s\\S]*?)</a>",
      std::regex::icase);
  static const std::regex kAnchorSq(
      "<a\\b[^>]*\\bhref\\s*=\\s*'([^']*)'[^>]*>([\\s\\S]*?)</a>",
      std::regex::icase);
  std::smatch m;
  if (std::regex_search(html, m, kAnchorDq)) {
    const std::string href = m[1].str();
    const std::string text = m[2].str();
    const std::string repl = is_safe_description_url(href)
                                 ? ("[url=" + href + "]" + text + "[/url]")
                                 : text;
    html.replace(m.position(), m.length(), repl);
    return true;
  }
  if (std::regex_search(html, m, kAnchorSq)) {
    const std::string href = m[1].str();
    const std::string text = m[2].str();
    const std::string repl = is_safe_description_url(href)
                                 ? ("[url=" + href + "]" + text + "[/url]")
                                 : text;
    html.replace(m.position(), m.length(), repl);
    return true;
  }
  return false;
}

// Strip every HTML tag from `html` EXCEPT <a>, <br>, and <p> which are
// converted to BBCode equivalents (the anchor pass already converted
// <a>). Other tags - IPS chrome (<span>, <strong>, etc.) - are
// stripped and their inner text preserved. Runs of >2 newlines (the
// <p> pass introduces them) are collapsed to one paragraph break.
void strip_unwanted_tags(std::string &html) {
  // <br> -> '\n' (case-insensitive, flexible on trailing slash / ws).
  static const std::regex kBr("<\\s*br\\s*/?\\s*>", std::regex::icase);
  html = std::regex_replace(html, kBr, "\n");
  // <p ...>...</p> -> "\n\n" + inner. Open <p> tags without a matching
  // close (malformed HTML) are dropped by the kAnyTag pass below.
  static const std::regex kP(
      "<p\\b[^>]*>([\\s\\S]*?)</p>", std::regex::icase);
  html = std::regex_replace(html, kP, "\n\n$1\n\n");
  // Any remaining tag is dropped.
  static const std::regex kAnyTag("<[^>]+>");
  html = std::regex_replace(html, kAnyTag, "");
  // Collapse runs of >2 newlines.
  static const std::regex kThreeNl("\n{3,}");
  html = std::regex_replace(html, kThreeNl, "\n\n");
  // Trim surrounding whitespace.
  auto first = html.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    html.clear();
    return;
  }
  auto last = html.find_last_not_of(" \t\r\n");
  html = html.substr(first, last - first + 1);
}

// Public: extract the first "About This File" rich-text block from the
// page HTML and return a BBCode-ish string the UI's bbcode_to_html can
// further normalize (CRLF, dedup, autolink bare URLs, linkify @mentions).
// The block is identified by class="ipsType_richText" - that's the
// Invision Community content area for the file description. We pick the
// FIRST such block because the changelog (also ipsType_richText) sits
// further down the page and we want the main description, not the
// changelog. (Future work: skip the block whose preceding <h2> says
// "What's New" / "Changelog" - not in this PR.)
std::string extract_rich_description(const std::string &html_body) {
  if (html_body.empty())
    return {};
  // Locate the first `<div ...class="...ipsType_richText..." ...>` open
  // tag. We do not bother matching the full `<div ...>...</div>` because
  // nesting is unreliable; once we have the open we walk to the next
  // `</div>` at the same depth by counting opens. That keeps the
  // matching depth-aware in the simple case where ipsType_richText
  // contains inline `<div>`s for embeds.
  // Match either single- or double-quoted class attribute - LL pages
  // have rendered both shapes over the years (and mod.pub sometimes
  // uses single quotes). The character class ["'] matches the open
  // quote, the back-ref via ["'] keeps the close quote the same.
  static const std::regex kOpen(
      "<div\\b[^>]*\\bclass\\s*=\\s*[\"'][^\"']*ipsType_richText[^\"']*[\"'][^>]*>",
      std::regex::icase);
  std::smatch m;
  if (!std::regex_search(html_body, m, kOpen))
    return {};
  const size_t open_end = m.position() + m.length();
  // Depth-aware walk to the matching </div>.
  size_t depth = 1;
  size_t pos = open_end;
  static const std::regex kDivOpen("<div\\b", std::regex::icase);
  static const std::regex kDivClose("</div\\s*>", std::regex::icase);
  while (depth > 0 && pos < html_body.size()) {
    auto find_next = [&](const std::regex &re) -> size_t {
      const std::string rest = html_body.substr(pos);
      std::smatch r;
      return std::regex_search(rest, r, re) ? size_t(r.position()) + pos
                                            : std::string::npos;
    };
    const size_t next_open = find_next(kDivOpen);
    const size_t next_close = find_next(kDivClose);
    if (next_close == std::string::npos)
      break; // malformed; bail out
    if (next_open != std::string::npos && next_open < next_close) {
      ++depth;
      pos = next_open + std::strlen("<div");
    } else {
      --depth;
      pos = next_close + std::strlen("</div");
      if (depth == 0) {
        // `pos` is one past the closing </div>. Inner HTML is
        // [open_end, pos - strlen("</div")).
        std::string inner = html_body.substr(
            open_end, (pos - std::strlen("</div")) - open_end);
        // Convert <a> -> [url] (or drop unsafe). The anchor pass
        // operates on a copy of `inner` to keep the regex state
        // local to the function.
        while (convert_anchor_once(inner)) {
          // Loop until no more anchors. Bounded by the anchor count
          // in the document.
        }
        strip_unwanted_tags(inner);
        return inner;
      }
    }
  }
  return {};
}

} // namespace

std::string Provider::parse_description_html(const std::string &html_body) {
  return extract_rich_description(html_body);
}

ModInfoResult Provider::parse_mod_info(const std::string &html_body) {
  ModInfoResult result;
  if (html_body.empty())
    return result;

  // --- Step 1: prefer the schema.org WebApplication JSON-LD block. ---
  std::string ld_body = find_webapp_json_ld(html_body);
  if (!ld_body.empty()) {
    ld_body = extract_webapp_object(ld_body);
    try {
      auto j = nlohmann::json::parse(ld_body);
      if (j.is_object()) {
        result.name = j_str(j, "name");
        result.version = j_str(j, "softwareVersion");
        result.category = j_str(j, "applicationCategory");
        result.description = j_str(j, "description");
        result.date_modified = j_str(j, "dateModified");
        // author can be an object {name,url} or a bare string.
        if (j.contains("author")) {
          const auto &a = j.at("author");
          if (a.is_object())
            result.author = j_str(a, "name");
          else if (a.is_string())
            result.author = trim(a.get<std::string>());
        }
        // url: canonical /files/file/{id}/ (or whatever the page
        // advertised). Stored verbatim - the visit fallback in the panel
        // adds its own trailing slash when needed.
        auto url = j_str(j, "url");
        if (!url.empty())
          result.page_url = url;
      }
    } catch (const std::exception &) {
      // Malformed JSON-LD - drop everything and fall through to og:*.
      result = {};
    }
  }

  // --- Step 2: ALWAYS prefer the rich-text description block over
  // the JSON-LD plain-text one when we can extract it. The Invision
  // Community content area (class="ipsType_richText", the "About This
  // File" container) carries the real HTML the user sees: <a> anchors,
  // <strong>, mentions. JSON-LD's `description` is plain text by the
  // schema.org contract, so any link or mention in the on-site view is
  // lost when we use it. parse_description_html() converts the inner
  // HTML to a BBCode-ish string the UI's bbcode_to_html can further
  // normalize (CRLF, dedup, autolink, @mentions).
  // The previous gate (`if (result.description.empty())`) made the
  // rich path a fallback that almost never ran - JSON-LD is present
  // on every LL page - so links/mentions stayed lost. Spec says
  // prefer rich when available; this does that unconditionally.
  const std::string rich = parse_description_html(html_body);
  if (!rich.empty())
    result.description = rich;

  // --- Step 3: fallback to og:* meta tags. ---
  // Used when JSON-LD is absent or did not produce a name. Description and
  // dateModified remain empty when og:* does not carry them (IPS pages do
  // not advertise og:updated_time uniformly), and the caller's "available"
  // gate (name AND description) then falls back to false.
  if (result.name.empty())
    result.name = read_meta(html_body, "og:title");
  if (result.description.empty())
    result.description = read_meta(html_body, "og:description");
  if (result.author.empty())
    result.author = read_meta(html_body, "article:author");
  if (result.date_modified.empty()) {
    // og:updated_time is the usual fall-back. Invision Community does
    // not always emit it, in which case we have no date signal at all.
    result.date_modified = read_meta(html_body, "og:updated_time");
  }

  // available gate: the page must have advertised both a name and at
  // least a description; an empty result is indistinguishable from a
  // failure (network/parse) and should not be treated as success.
  result.available = !result.name.empty() && !result.description.empty();
  return result;
}

ModInfoResult
Provider::fetch_mod_info(const std::string &file_id_or_url) {
  ModInfoResult result;
  if (file_id_or_url.empty())
    return result;

  // Accept either a full LoversLab URL or a bare numeric file id. Reject
  // anything that does not look like an id before spending an HTTP call.
  std::string url;
  if (file_id_or_url.find("://") != std::string::npos) {
    if (!is_loverslab_url(file_id_or_url)) {
      Logger::instance().debug(
          "LoversLabProvider: fetch_mod_info got a non-LoversLab URL");
      return result;
    }
    url = mod_page_url(file_id_or_url);
    if (url.empty())
      return result;
  } else {
    // std::all_of on an empty range returns true, so the empty case is
    // already covered - no separate `!empty()` guard needed.
    const bool digits_only = std::all_of(
        file_id_or_url.begin(), file_id_or_url.end(),
        [](unsigned char c) { return std::isdigit(c); });
    if (!digits_only) {
      Logger::instance().debug(
          "LoversLabProvider: fetch_mod_info id is not numeric");
      return result;
    }
    url = build_page_url(file_id_or_url);
  }

  // Route the guest scrape through Network::. Metadata is guest-visible; we
  // deliberately do NOT send the user's session cookie here. Downloads
  // require cookie auth (handled in Provider::fetch); metadata does not.
  // Sending the cookie to a GET would tie the request to a Cloudflare
  // cf_clearance fingerprint and may trigger a captcha challenge.
  network::Request req;
  req.url = Http::encode_url_path(url);
  req.caller = NET_CALLER;
  req.timeout = std::chrono::seconds(15);
  req.follow_redirect = true;
  // Cap the response body. LoversLab mod pages are well under 100 KB; the
  // write callback aborts the transfer if append_body() exceeds kMaxBodyBytes.
  // Belt-and-braces: Network:: also enforces a separate ceiling via
  // max_bytes so a misconfigured server cannot push us into OOM.
  req.max_bytes = static_cast<std::int64_t>(kMaxBodyBytes);
  req.headers.push_back(
      "User-Agent: GameModManager/0.1 (LoversLab Provider)");

  auto resp = network::instance().request(req);
  if (!resp.error.empty()) {
    Logger::instance().debug(
        "LoversLabProvider: fetch_mod_info curl error: " + resp.error);
    return result;
  }
  if (resp.http_code != 200) {
    Logger::instance().debug(
        "LoversLabProvider: fetch_mod_info HTTP " +
        std::to_string(resp.http_code) +
        " - site may require a browser or the mod id is invalid");
    return result;
  }

  result = parse_mod_info(resp.body);
  // Pin page_url to the URL we fetched (parse_mod_info may have set it
  // from the JSON-LD `url` field, which usually agrees but is not
  // guaranteed).
  if (result.page_url.empty())
    result.page_url = url;
  if (result.available) {
    Logger::instance().debug(
        "LoversLabProvider: fetched mod info for " + url + " (date_modified: " +
        (result.date_modified.empty() ? "<none>" : result.date_modified) + ")");
  }
  return result;
}

} // namespace engine::Source::LoversLab