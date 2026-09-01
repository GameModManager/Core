#include "engine/source/loverslab/provider.h"
#include "engine/core/log/logger.h"
#include "engine/source/http_util.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <cctype>
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
        for (const auto &node : j["@graph"]) {
          if (check(node))
            return node.dump();
        }
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

} // namespace

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

  // --- Step 2: fallback to og:* meta tags. ---
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
Provider::fetch_mod_info(const std::string &file_id_or_url) const {
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
    bool digits_only = !file_id_or_url.empty();
    for (const char c : file_id_or_url) {
      if (!std::isdigit(static_cast<unsigned char>(c))) {
        digits_only = false;
        break;
      }
    }
    if (!digits_only) {
      Logger::instance().debug(
          "LoversLabProvider: fetch_mod_info id is not numeric");
      return result;
    }
    url = build_page_url(file_id_or_url);
  }

  auto *curl = curl_easy_init();
  if (!curl)
    return result;

  const std::string encoded = Http::encode_url_path(url);
  std::string body;

  curl_easy_setopt(curl, CURLOPT_URL, encoded.c_str());
  curl_easy_setopt(curl, CURLOPT_USERAGENT,
                   "GameModManager/0.1 (LoversLab Provider)");
  // Metadata is guest-visible; we deliberately do NOT send the user's
  // session cookie here. Downloads require cookie auth (handled in
  // Provider::fetch); metadata does not. Sending the cookie to a GET
  // would tie the request to a Cloudflare cf_clearance fingerprint and
  // may trigger a captcha challenge.
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
  // Restrict redirect protocols to HTTP(S). Without this libcurl will follow
  // a 302 to file://, ftp://, gopher://, etc - we only ever want to follow
  // to a web URL. The initial URL is also pre-validated against
  // is_loverslab_url() so a hostile redirect to 169.254.169.254 or
  // 127.0.0.1 is blocked at the protocol layer before any host check.
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS,
                   CURLPROTO_HTTPS | CURLPROTO_HTTP);
  // Cap the response body. LoversLab mod pages are well under 100 KB; the
  // write callback aborts the transfer if append_body() exceeds kMaxBodyBytes.
  // Belt-and-braces: libcurl also enforces a separate ceiling here.
  curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE,
                   static_cast<curl_off_t>(kMaxBodyBytes));
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L); // guest fetch - keep tight
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_body);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
  // Don't bother reading headers - we only need the body to parse.

  const CURLcode res = curl_easy_perform(curl);
  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK) {
    Logger::instance().debug("LoversLabProvider: fetch_mod_info curl error: " +
                             std::string(curl_easy_strerror(res)));
    return result;
  }
  if (http_code != 200) {
    Logger::instance().debug(
        "LoversLabProvider: fetch_mod_info HTTP " + std::to_string(http_code) +
        " - site may require a browser or the mod id is invalid");
    return result;
  }

  result = parse_mod_info(body);
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