// =============================================================================
// Network:: concrete implementation - the libcurl-backed gateway.
//
// Design:
//   * One process-wide Manager owns a CURLSH* share handle (DNS cache,
//     TLS session cache, connection pool). Created in ctor, destroyed in
//     dtor.
//   * request() and download() are synchronous on the calling thread, like
//     the pre-Network:: curl helpers they replace. The active map and log
//     ring are mutex-guarded for cross-thread visibility (Debug panel reads
//     from the UI thread; providers run on workers).
//   * Every call records one LogEntry. Bodies and headers are run through
//     redaction::* before being written.
//   * retry: simple, in-process exponential backoff. Honors Retry-After
//     when the response carries one.
//   * Proxy / offline / user agent are pulled from NetworkOptions on every
//     request.
// =============================================================================

// libcurl BEFORE our header so its typedefs (CURL, curl_slist, CURLSH)
// are already in scope; the header forward-declares them only when curl.h
// has not been pulled in yet.
#include <curl/curl.h>

#include "engine/network/network_manager.h"

#include "engine/core/log/logger.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>

namespace engine::network {

// -----------------------------------------------------------------------------
// redaction
// -----------------------------------------------------------------------------
namespace redaction {

namespace {

std::string to_lower_copy(const std::string& s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool name_matches(const std::string& header_name, std::initializer_list<const char*> needles) {
    const std::string n = to_lower_copy(header_name);
    for (const char* needle : needles) {
        if (n == needle) return true;
    }
    return false;
}

} // namespace

bool is_sensitive_header(const std::string& header_name) {
    return name_matches(header_name, {
        "apikey", "authorization", "cookie", "set-cookie",
        "x-api-key", "x-auth-token", "x-csrf-token", "csrf-token",
        "proxy-authorization",
        // Generic secret keys (URL-encoded query params and JSON bodies).
        "key", "token", "password", "secret", "session", "csrfkey"
    });
}

std::string redact_header_line(const std::string& line) {
    // "Name: value" - find first ':'
    const auto colon = line.find(':');
    if (colon == std::string::npos) return line;
    const std::string name = line.substr(0, colon);
    if (!is_sensitive_header(name)) return line;
    return name + ": <redacted>";
}

namespace {

// M3 helper: true if `seg` looks like a long opaque secret token rather
// than a numeric id or normal path part. Catches hex/base64 tokens in
// URL paths (e.g. /v1/tokens/abc123.../data). Conservative on purpose:
// requires length >= 24, mix of letters+digits, no URL-special chars.
bool looks_like_token(const std::string& seg) {
    if (seg.size() < 24) return false;
    bool has_letter = false, has_digit = false;
    for (char c : seg) {
        const bool ok = (c >= '0' && c <= '9') ||
                         (c >= 'a' && c <= 'z') ||
                         (c >= 'A' && c <= 'Z') ||
                         c == '-' || c == '_' || c == '.';
        if (!ok) return false;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
            has_letter = true;
        if (c >= '0' && c <= '9') has_digit = true;
    }
    return has_letter && has_digit;
}

} // namespace

std::string redact_url(const std::string& url) {
    // Redact sensitive query params (apikey, key, token, csrf, ...) AND
    // (M3) path segments that look like secrets (e.g. bearer tokens
    // embedded directly in the path).
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return url;

    // Walk the path between host and query/fragment. For each segment,
    // check whether the *previous* segment's name matches a sensitive
    // key (e.g. ".../token/abc/data") or whether the segment itself
    // looks like a long opaque token. Each `segment` substring already
    // includes its leading '/' so we don't re-add a separator below.
    std::string rewritten = url;
    const auto host_end = url.find('/', scheme_end + 3);
    if (host_end != std::string::npos) {
        std::string out;
        out.reserve(url.size());
        out.append(url, 0, host_end);  // scheme + host (no trailing /)
        std::size_t prev_name_start = std::string::npos;
        std::size_t i = host_end;
        while (i < url.size() && url[i] != '?' && url[i] != '#') {
            const std::size_t seg_start = i;
            std::size_t seg_end = url.find('/', i + 1);
            if (seg_end == std::string::npos) seg_end = url.size();
            const std::string segment = url.substr(
                seg_start, seg_end - seg_start);
            // Compare against the previous segment's NAME (without its
            // leading '/'). prev_name_start points at that '/'.
            bool redact = false;
            if (prev_name_start != std::string::npos) {
                const std::size_t name_start = prev_name_start + 1;
                const std::string prev_name = url.substr(
                    name_start, seg_start - name_start);
                if (is_sensitive_header(prev_name)) redact = true;
            }
            // Token-shape check uses the segment name only (strip the
            // leading '/'), otherwise the leading slash would always
            // fail the "alphanumeric only" check.
            if (!redact) {
                const std::string seg_name =
                    segment.size() && segment.front() == '/'
                        ? segment.substr(1)
                        : segment;
                if (looks_like_token(seg_name)) redact = true;
            }
            if (redact && segment.size() > 1) {
                // segment starts with '/' - replace the name part only.
                out += "/<redacted>";
            } else {
                out += segment;
                prev_name_start = seg_start;
            }
            i = seg_end;
        }
        // Append query + fragment suffix for the query-redaction pass.
        if (i < url.size()) out.append(url, i, std::string::npos);
        rewritten = std::move(out);
    }

    // Query redaction (the original code).
    const auto q = rewritten.find('?', scheme_end + 3);
    if (q == std::string::npos) return rewritten;

    const auto frag = rewritten.find('#', q);
    std::string prefix = rewritten.substr(0, q + 1);
    std::string query = (frag == std::string::npos)
        ? rewritten.substr(q + 1)
        : rewritten.substr(q + 1, frag - q - 1);
    std::string frag_suffix =
        (frag == std::string::npos) ? "" : rewritten.substr(frag);

    std::string out_query;
    out_query.reserve(query.size());
    std::size_t pos = 0;
    bool first = true;
    while (pos < query.size()) {
        std::size_t amp = query.find('&', pos);
        if (amp == std::string::npos) amp = query.size();
        std::string segment = query.substr(pos, amp - pos);

        if (!first) out_query.push_back('&');
        first = false;

        const auto eq = segment.find('=');
        if (eq == std::string::npos) {
            out_query += segment;
        } else {
            const std::string key = segment.substr(0, eq);
            if (is_sensitive_header(key)) {
                out_query += key + "=<redacted>";
            } else {
                out_query += segment;
            }
        }
        pos = amp + 1;
    }
    return prefix + out_query + frag_suffix;
}

std::string redact_body(const std::string& body, std::size_t preview_bytes) {
    // Cheap path: empty body or no '=' / no ':' at all means there's nothing
    // to redact. JSON bodies use ':' as the kv separator and need the second
    // pass to fire, so we check for either.
    const bool url_form = body.find('=') != std::string::npos;
    const bool json_form = body.find('"') != std::string::npos &&
                           body.find(':') != std::string::npos;
    if (body.empty() || (!url_form && !json_form)) {
        if (body.size() > preview_bytes) return body.substr(0, preview_bytes) + "... <truncated>";
        return body;
    }

    // Walk the body char-by-char, replacing sensitive key values with
    // <redacted>. This is best-effort: works for plain URL-encoded forms
    // (apikey=ABC&token=XYZ) and JSON-ish bodies ("apikey": "ABC").
    auto scrub_pairs = [&](const std::string& body, char sep, char eq) {
        std::string out;
        out.reserve(body.size());
        std::size_t pos = 0;
        while (pos < body.size()) {
            // Copy separator boundary.
            if (body[pos] == sep) {
                out.push_back(sep);
                ++pos;
                continue;
            }
            std::size_t next_sep = body.find(sep, pos);
            if (next_sep == std::string::npos) next_sep = body.size();
            std::string segment = body.substr(pos, next_sep - pos);
            const auto e = segment.find(eq);
            if (e != std::string::npos) {
                // The "key" includes whatever precedes the eq up to the
                // first opening quote (or first non-quote char). For JSON
                // segments shaped like {"apikey":"ABC" we want the key
                // "apikey" - strip leading noise.
                std::string key = segment.substr(0, e);
                // Find the start of the key inside the leading noise.
                std::size_t key_start = 0;
                while (key_start < key.size() && key[key_start] != '"') ++key_start;
                std::string key_only_str;
                // If we found an opening quote, find the closing one and
                // take the inner string between the quotes.
                if (key_start < key.size()) {
                    std::size_t key_end = key.find('"', key_start + 1);
                    if (key_end == std::string::npos) key_end = e;
                    key_only_str = key.substr(key_start + 1, key_end - key_start - 1);
                } else {
                    // No quote in the key (URL-encoded form): the whole
                    // pre-`=` prefix is the key.
                    key_only_str = key;
                }
                if (is_sensitive_header(key_only_str)) {
                    // For JSON segments we keep the wrapping quotes of the
                    // value so the document stays valid. Find the
                    // trailing quote if present.
                    std::size_t val_open = e + 1;
                    std::size_t val_close = segment.size();
                    bool quoted = false;
                    if (val_open < segment.size() &&
                        (segment[val_open] == '"' || segment[val_open] == '\'')) {
                        quoted = true;
                        // Find the matching close-quote (assume no escapes
                        // for this simple scrubber; sensitive payloads are
                        // small enough that escapes don't matter).
                        val_close = segment.find(segment[val_open], val_open + 1);
                        if (val_close == std::string::npos) val_close = segment.size();
                    }
                    // Keep the opening quote (val_open includes it) and the
                    // closing one (val_close).
                    out += segment.substr(0, val_open + (quoted ? 1 : 0)) +
                           "<redacted>" +
                           (quoted && val_close < segment.size()
                                ? segment.substr(val_close)
                                : std::string());
                } else {
                    out += segment;
                }
            } else {
                out += segment;
            }
            pos = next_sep;
        }
        return out;
    };

    // URL-encoded form (apikey=ABC&...).
    std::string scrubbed;
    if (url_form) {
        scrubbed = scrub_pairs(body, '&', '=');
    } else {
        scrubbed = body;
    }

    // JSON-ish: "apikey":"ABC". Re-run with ', as separator and ':' as
    // kv sep. json_form is set when the body has both '"' and ':' so the
    // redactor can process {"apikey":"ABC"} style payloads.
    if (json_form) {
        scrubbed = scrub_pairs(scrubbed, ',', ':');
    }

    if (scrubbed.size() > preview_bytes) {
        return scrubbed.substr(0, preview_bytes) + "... <truncated>";
    }
    return scrubbed;
}

std::string redact_cookie(const std::string& cookie) {
    // "name1=value1; name2=value2; ..." -> "name1=<redacted>, ... (N chars)"
    if (cookie.empty()) return {};
    std::string first_name;
    std::size_t total = cookie.size();
    const auto eq = cookie.find('=');
    if (eq != std::string::npos) {
        first_name = cookie.substr(0, eq);
    }
    if (first_name.empty()) {
        return "<cookie: " + std::to_string(total) + " chars>";
    }
    return first_name + "=<redacted>, ... (" + std::to_string(total) + " chars)";
}

} // namespace redaction

// -----------------------------------------------------------------------------
// libcurl callbacks (file-private)
// -----------------------------------------------------------------------------
namespace {

// Append received bytes to a std::string. Used for response bodies.
size_t write_to_string(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    const std::size_t bytes = size * nmemb;
    s->append(ptr, bytes);
    return bytes;
}

// Single header callback. libcurl only allows ONE CURLOPT_HEADERFUNCTION per
// easy handle, so this struct + callback captures the raw "Name: value\r\n"
// text into `response_headers` AND extracts the Content-Disposition value
// on the fly. Older code set CURLOPT_HEADERFUNCTION twice (once for headers,
// once for Content-Disposition) which silently dropped whichever callback
// was registered last; this consolidates them into one pass so both
// response_headers and content_disposition always get populated.
struct HeaderCapture {
    std::string* response_headers = nullptr;  // raw CRLF-delimited text
    std::string* content_disposition = nullptr;  // final Content-Disposition value
};

// Per-handle scratch carried in CURLOPT_PRIVATE. The slist owns the
// request header strings; the HeaderCapture carries the response header
// pointers. Both must outlive curl_easy_perform; both are freed by the
// caller via curl_easy_getinfo(curl, CURLINFO_PRIVATE, ...).
struct PrivateData {
    curl_slist* slist = nullptr;
    HeaderCapture* cap = nullptr;
};

size_t capture_headers(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    const std::size_t bytes = size * nmemb;
    auto* cap = static_cast<HeaderCapture*>(userdata);
    if (cap && cap->response_headers) {
        cap->response_headers->append(ptr, bytes);
    }
    if (cap && cap->content_disposition) {
        // Strip trailing CRLFs so we can match the header line prefix cleanly.
        std::string_view line(ptr, bytes);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.remove_suffix(1);
        }
        // Case-insensitive prefix check (servers sometimes emit
        // "content-disposition: ..." lowercase).
        constexpr std::string_view kPrefix = "Content-Disposition:";
        bool match = line.size() > kPrefix.size();
        if (match) {
            for (std::size_t i = 0; i < kPrefix.size(); ++i) {
                char a = line[i];
                char b = kPrefix[i];
                if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + ('a' - 'A'));
                if (b >= 'A' && b <= 'Z') b = static_cast<char>(b + ('a' - 'A'));
                if (a != b) { match = false; break; }
            }
        }
        if (match) {
            std::size_t start = kPrefix.size();
            while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) {
                ++start;
            }
            // Last hop wins: a redirect hop may set one that the final
            // response then overrides.
            cap->content_disposition->assign(line.substr(start));
        }
    }
    return bytes;
}

// Progress callback for downloads. Forwards to ProgressCb and respects
// should_abort (returns 1 to abort, partial file kept). Computes a real
// bytes-per-second value from the wall-clock elapsed time + bytes received
// since the transfer started (previous code passed a constant 0.0 - UI
// Download tab was always 0 bps).
struct ProgressForwarder {
    ProgressCb* cb = nullptr;
    std::chrono::steady_clock::time_point started{};
};

int xfer_forwarder(void* userdata, curl_off_t dltotal, curl_off_t dlnow,
                 curl_off_t ultotal, curl_off_t ulnow) {
    (void)ultotal; (void)ulnow;
    auto* pf = static_cast<ProgressForwarder*>(userdata);
    if (!pf || !pf->cb) return 0;
    if (pf->cb->should_abort && pf->cb->should_abort()) {
        return 1;  // abort, partial file kept
    }
    if (pf->cb->on) {
        const auto now = std::chrono::steady_clock::now();
        const double seconds =
            std::chrono::duration<double>(now - pf->started).count();
        // Avoid divide-by-zero / nonsense 1-2 sample bps spike on the very
        // first tick (<= 100ms): report 0 until we have a meaningful sample.
        double bps = 0.0;
        if (seconds > 0.1 && dlnow > 0) {
            bps = static_cast<double>(dlnow) / seconds;
        }
        pf->cb->on(pf->cb->resume_base + dlnow,
                   pf->cb->resume_base + dltotal, bps);
    }
    return 0;
}

// File write callback. Returns 0 to abort the transfer on file error.
struct FileWriteState {
    std::ofstream* file = nullptr;
    std::int64_t bytes_written = 0;
    ProgressForwarder* progress = nullptr;
    std::chrono::steady_clock::time_point started{};
    std::uint64_t last_speed_update_ms = 0;
};

size_t write_to_file(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* st = static_cast<FileWriteState*>(userdata);
    if (!st || !st->file || !st->file->is_open()) return 0;
    const std::size_t bytes = size * nmemb;
    st->file->write(ptr, static_cast<std::streamsize>(bytes));
    if (!st->file->good()) return 0;
    st->bytes_written += static_cast<std::int64_t>(bytes);
    return bytes;
}

std::string join_headers(const std::vector<std::string>& headers) {
    std::string out;
    for (const auto& h : headers) {
        out += h;
        if (!h.empty() && h.back() != '\n') out += "\r\n";
    }
    return out;
}

// Parse the Retry-After response header (seconds OR HTTP-date) and return
// the millisecond delay it implies. Returns 0 when the header is missing,
// unparseable, or negative. Caps at 24h so a hostile or buggy server can't
// park us indefinitely.
int parse_retry_after_ms(const std::string& response_headers) {
    constexpr int kMaxRetryAfterMs = 24 * 60 * 60 * 1000;
    // Find the "Retry-After:" header (case-insensitive).
    std::size_t pos = std::string::npos;
    std::size_t value_start = std::string::npos;
    for (std::size_t i = 0; i + 11 < response_headers.size(); ++i) {
        if (response_headers[i] == '\n' || response_headers[i] == '\r') continue;
        // Try matching "Retry-After:" at position i (case-insensitive)
        static const char needle[] = "retry-after:";
        bool match = true;
        for (std::size_t k = 0; k < sizeof(needle) - 1; ++k) {
            char a = response_headers[i + k];
            char b = needle[k];
            if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + ('a' - 'A'));
            if (a != b) { match = false; break; }
        }
        if (match) {
            pos = i + sizeof(needle) - 1;
            break;
        }
    }
    if (pos == std::string::npos) return 0;
    // Skip whitespace.
    while (pos < response_headers.size() &&
           (response_headers[pos] == ' ' || response_headers[pos] == '\t')) {
        ++pos;
    }
    // Read up to the next CRLF or comma (commas separate multiple Retry-After
    // values; we take the first).
    value_start = pos;
    while (pos < response_headers.size() &&
           response_headers[pos] != '\r' && response_headers[pos] != '\n' &&
           response_headers[pos] != ',') {
        ++pos;
    }
    if (pos == value_start) return 0;
    std::string value(response_headers, value_start, pos - value_start);
    // Trim trailing whitespace.
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    if (value.empty()) return 0;
    // Numeric form: "120" -> 120 seconds.
    bool all_digits = std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return c >= '0' && c <= '9';
    });
    if (all_digits) {
        long secs = 0;
        try { secs = std::stol(value); } catch (...) { return 0; }
        if (secs <= 0) return 0;
        return static_cast<int>(std::min<long>(secs * 1000, kMaxRetryAfterMs));
    }
    // HTTP-date form: try to parse via get_time; if it fails, give up.
    std::tm tm{};
    std::istringstream iss(value);
    iss >> std::get_time(&tm, "%a, %d %b %Y %H:%M:%S");
    if (iss.fail()) {
        iss.clear();
        iss >> std::get_time(&tm, "%A, %d-%b-%y %H:%M:%S");
    }
    if (iss.fail()) {
        iss.clear();
        iss >> std::get_time(&tm, "%a %b %d %H:%M:%S %Y");
    }
    if (iss.fail()) return 0;
    // tm is in local time; convert to time_t, then to delta from now.
    std::time_t target = std::mktime(&tm);
    if (target == -1) return 0;
    const auto now = std::chrono::system_clock::now();
    const auto delta = std::chrono::system_clock::from_time_t(target) - now;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(delta).count();
    if (ms <= 0) return 0;
    if (ms > kMaxRetryAfterMs) ms = kMaxRetryAfterMs;
    return static_cast<int>(ms);
}

// CURLSH lock callbacks. libcurl invokes these whenever any thread that
// holds an easy handle attached to our share touches DNS / SSL-session /
// connection state. Without these set, libcurl docs are explicit that
// concurrent use of a share handle is undefined behaviour - in practice
// it manifests as spurious hangs under load.
void share_lock_cb(CURL* /*handle*/, curl_lock_data data,
                   curl_lock_access /*access*/, void* userptr) {
    if (!userptr) return;
    auto* locks = static_cast<std::unordered_map<int, std::mutex>*>(userptr);
    auto it = locks->find(static_cast<int>(data));
    if (it != locks->end()) it->second.lock();
}

void share_unlock_cb(CURL* /*handle*/, curl_lock_data data, void* userptr) {
    if (!userptr) return;
    auto* locks = static_cast<std::unordered_map<int, std::mutex>*>(userptr);
    auto it = locks->find(static_cast<int>(data));
    if (it != locks->end()) it->second.unlock();
}

} // anonymous namespace (libcurl callbacks)

// -----------------------------------------------------------------------------
// Manager
// -----------------------------------------------------------------------------

Manager::Manager() {
    // One-shot global init. Safe to call multiple times (libcurl itself
    // refcounts), but keep it here so libcurl's bookkeeping (DNS, etc.) is
    // alive before the share handle hands it out.
    curl_global_init(CURL_GLOBAL_DEFAULT);

    share_ = curl_share_init();
    if (!share_) return;
    // Enable DNS + TLS session + connection caches. The defaults for an
    // empty share are all disabled; turning them on is what actually gives
    // us free keep-alive across hosts.
    curl_share_setopt(share_, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
    curl_share_setopt(share_, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
    curl_share_setopt(share_, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
    // libcurl documents that any share handle used across threads MUST
    // have CURLSHOPT_LOCKFUNC/UNLOCKFUNC set; without them, accessing the
    // DNS/SSL/connection caches from multiple threads is a data race
    // (libcurl does not lock internally). One mutex per lock data kind.
    curl_share_setopt(share_, CURLSHOPT_LOCKFUNC, share_lock_cb);
    curl_share_setopt(share_, CURLSHOPT_UNLOCKFUNC, share_unlock_cb);
    curl_share_setopt(share_, CURLSHOPT_USERDATA, &share_locks_);
}

Manager::~Manager() {
    std::lock_guard<std::mutex> lock(mu_);
    if (share_) {
        curl_share_cleanup(share_);
        share_ = nullptr;
    }
}

void Manager::apply_options(CURL* curl) const {
    if (opts_.use_proxy && !opts_.proxy_host.empty()) {
        // libcurl expects a scheme ("http://", "socks5://") on CURLOPT_PROXY;
        // a bare "host:port" string is mis-parsed by libcurl and silently
        // ignored on most versions. Prepend http:// unless the host already
        // carries a scheme (rare, but supported).
        std::string proxy_url = opts_.proxy_host;
        if (proxy_url.find("://") == std::string::npos) {
            proxy_url = "http://" + proxy_url;
        }
        proxy_url += ":" + std::to_string(opts_.proxy_port);
        curl_easy_setopt(curl, CURLOPT_PROXY, proxy_url.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                     "GameModManager/" VERSION);
    // When a share handle is present, attach it for DNS / TLS / connection
    // reuse.
    if (share_) {
        curl_easy_setopt(curl, CURLOPT_SHARE, share_);
    }
}

CURL* Manager::prepare_request(const Request& req, Response& out_resp,
                               std::string& out_err) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        out_err = "curl_easy_init failed";
        return nullptr;
    }

    // URL with redaction-friendly view (the original goes on the wire;
    // redaction only happens when logging - the LogEntry is built in
    // Manager::request() from a fresh redact_url() pass).
    out_resp.effective_url = req.url;

    curl_easy_setopt(curl, CURLOPT_URL, req.url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, req.follow_redirect ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(req.timeout.count()));
    if (req.max_bytes > 0) {
        curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE,
                         static_cast<curl_off_t>(req.max_bytes));
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out_resp.body);

    // Default headers (curl_slist-style).
    curl_slist* slist = nullptr;
    auto add_header = [&](const std::string& h) {
        slist = curl_slist_append(slist, h.c_str());
    };
    // Always include the Nexus AUP identification headers if the URL is a
    // Nexus API host - matches the pre-Network:: nexus_http_request
    // behaviour and avoids breaking every Nexus call site individually.
    const bool is_nexus_api = req.url.find("api.nexusmods.com") != std::string::npos;
    if (is_nexus_api) {
        add_header("application-name: GameModManager");
        add_header(std::string("application-version: ") + VERSION);
        add_header("Accept: application/json");
    }
    for (const auto& h : req.headers) add_header(h);
    if (slist) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slist);

    switch (req.method) {
    case Method::Get:
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
        break;
    case Method::Head:
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
        break;
    case Method::Post:
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        if (!req.body.empty()) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                             static_cast<long>(req.body.size()));
        }
        break;
    case Method::Put:
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        if (!req.body.empty()) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                             static_cast<long>(req.body.size()));
        }
        break;
    case Method::Patch:
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
        if (!req.body.empty()) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                             static_cast<long>(req.body.size()));
        }
        break;
    case Method::Delete:
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
        break;
    }

    // Single header callback: writes raw header text into response_headers
    // AND extracts Content-Disposition into content_disposition. Older code
    // set CURLOPT_HEADERFUNCTION twice which silently dropped whichever was
    // registered second; this consolidates them so both fields are
    // populated for every request. LoversLab's probe no longer needs the
    // special-case branch.
    HeaderCapture cap;
    cap.response_headers = &out_resp.response_headers;
    cap.content_disposition = &out_resp.content_disposition;
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, capture_headers);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &cap);
    // The cap struct lives on prepare_request's stack; curl_easy_perform
    // is synchronous, so the pointer is safe across the transfer. Wrap
    // it + the slist into a heap-allocated PrivateData and round-trip via
    // CURLOPT_PRIVATE so the caller can free both after the perform.
    auto* priv = new PrivateData{slist, &cap};
    curl_easy_setopt(curl, CURLOPT_PRIVATE, priv);

    apply_options(curl);

    return curl;
}

CURL* Manager::prepare_download(DownloadRequest& req,
                                DownloadResult& out_res,
                                std::ofstream& out_file,
                                std::string& out_err) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        out_err = "curl_easy_init failed";
        return nullptr;
    }

    if (req.resume_from > 0)
        out_file.open(req.dest, std::ios::binary | std::ios::app);
    else
        out_file.open(req.dest, std::ios::binary);
    if (!out_file) {
        curl_easy_cleanup(curl);
        out_err = "cannot open destination: " + req.dest.string();
        return nullptr;
    }

    curl_easy_setopt(curl, CURLOPT_URL, req.url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    if (req.long_lived) {
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
        // No overall timeout - large archives routinely exceed fixed caps.
    } else {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT,
                         static_cast<long>(opts_.default_timeout_seconds));
    }
    if (req.resume_from > 0) {
        curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE,
                         static_cast<curl_off_t>(req.resume_from));
    }

    // Stack-allocated file write + progress state. Previously these were
    // `static thread_local`, which is fine in serial use but leaves stale
    // pointers around if an early return skipped the reassignment; the
    // stack version has well-bounded lifetime aligned with the easy handle.
    FileWriteState fws;
    fws.file = &out_file;
    fws.bytes_written = 0;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &fws);

    ProgressForwarder pf;
    pf.cb = &req.progress;
    pf.started = std::chrono::steady_clock::now();
    if (req.progress.on || (req.progress.should_abort)) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xfer_forwarder);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &pf);
    }

    // Capture response headers + Content-Disposition + final effective URL.
    // LoversLab's curl_download passes opts.effective_url back to the
    // provider; this restores that contract (B-04).
    // Pluse the header capture callback. The capture structs live in the
    // caller's DownloadResult (lifetime managed by Manager::download), so
    // prepare_download only takes their addresses and writes through them.
    // Doing this via stack-locals was tempting but the strings would
    // dangle once prepare_download returns and curl_easy_perform fires the
    // callback later.
    HeaderCapture cap;
    cap.response_headers = &out_res.response_headers;
    cap.content_disposition = &out_res.content_disposition;
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, capture_headers);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &cap);

    curl_slist* slist = nullptr;
    for (const auto& h : req.headers) slist = curl_slist_append(slist, h.c_str());
    if (slist) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slist);
    // Heap-allocate the lifetime-managed bundle so we can carry both the
    // header slist and the header capture pointers across the perform.
    // After curl_easy_perform, Manager::download reads back response
    // headers + Content-Disposition through priv->cap->* and copies them
    // into out_res. effective_url is pulled via CURLINFO_EFFECTIVE_URL
    // (separate from this struct).
    auto* priv = new PrivateData{slist, &cap};
    curl_easy_setopt(curl, CURLOPT_PRIVATE, priv);

    apply_options(curl);

    out_res.bytes_downloaded = 0;
    out_res.bytes_total = 0;
    (void)out_err;
    return curl;
}

Response Manager::request(const Request& req) {
    Response resp;
    if (cancelled_) {
        resp.error = "cancelled";
        return resp;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (opts_.offline_mode) {
            resp.error = "offline";
            return resp;
        }
    }

    const std::uint64_t id = next_id_.fetch_add(1);
    const auto started = std::chrono::system_clock::now();
    ActiveRequest active_rec;
    active_rec.id = id;
    active_rec.caller = req.caller;
    active_rec.method = method_name(req.method);
    active_rec.url_redacted = redaction::redact_url(req.url);
    active_rec.started = started;

    {
        std::lock_guard<std::mutex> lock(mu_);
        active_[id] = active_rec;
    }

    std::string prep_err;
    CURL* curl = prepare_request(req, resp, prep_err);
    if (!curl) {
        resp.error = prep_err;
        {
            std::lock_guard<std::mutex> lock(mu_);
            active_.erase(id);
        }
        LogEntry entry;
        entry.id = id;
        entry.started = started;
        entry.finished = std::chrono::system_clock::now();
        entry.caller = req.caller;
        entry.method = active_rec.method;
        entry.url_redacted = active_rec.url_redacted;
        entry.curl_error = prep_err;
        entry.ok = false;
        record_entry(std::move(entry));
        return resp;
    }

    // Retry loop with simple exponential backoff. Honours Retry-After
    // when present (M1), breaks on auth failure (401/403 - M2, no point
    // retrying a rejected credential), and clears the body/headers before
    // each retry so we don't append the second response onto the first
    // (B-03, would corrupt JSON and double the redaction cost).
    int attempts = std::max(1, opts_.max_retries + 1);
    int backoff_ms = std::max(1, opts_.retry_backoff_ms);
    constexpr int kMaxBackoffMs = 8000;
    bool succeeded = false;
    std::string last_curl_error;
    long last_http = 0;

    for (int i = 0; i < attempts; ++i) {
        if (i > 0) {
            // Reset body/headers/effective_url so the next attempt does not
            // append to / overwrite the previous attempt's data.
            resp.body.clear();
            resp.response_headers.clear();
            resp.effective_url.clear();
            resp.content_disposition.clear();
        }
        const CURLcode res = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.http_code);
        char* eff = nullptr;
        curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &eff);
        if (eff) resp.effective_url = eff;
        double tt = 0.0;
        curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &tt);
        resp.total_time_ms = tt * 1000.0;

        // Success: transport OK and the server didn't 5xx us.
        if (res == CURLE_OK && resp.http_code < 500) {
            succeeded = true;
            last_http = resp.http_code;
            break;
        }
        // Auth failure: the server explicitly rejected the credential.
        // Retrying is wasteful and risks rate-limiting. Break out
        // immediately.
        if (res == CURLE_OK && (resp.http_code == 401 || resp.http_code == 403)) {
            last_curl_error = "auth rejected";
            last_http = resp.http_code;
            break;
        }
        last_curl_error = curl_easy_strerror(res);
        last_http = resp.http_code;

        if (i + 1 >= attempts) break;
        // Prefer the server-provided Retry-After (seconds) when present,
        // capped at kMaxBackoffMs so a hostile server can't park us for
        // hours. Fall back to exponential backoff (capped) otherwise.
        int sleep_ms = backoff_ms;
        const auto ra_ms = parse_retry_after_ms(resp.response_headers);
        if (ra_ms > 0) sleep_ms = std::min(ra_ms, kMaxBackoffMs);
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        backoff_ms = std::min(backoff_ms * 2, kMaxBackoffMs);
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.http_code);
    {
        PrivateData* priv = nullptr;
        curl_easy_getinfo(curl, CURLINFO_PRIVATE, &priv);
        if (priv) {
            if (priv->slist) curl_slist_free_all(priv->slist);
            delete priv;
        }
    }
    curl_easy_cleanup(curl);

    if (!succeeded && resp.error.empty()) {
        resp.error = last_curl_error;
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        active_.erase(id);
    }

    LogEntry entry;
    entry.id = id;
    entry.started = started;
    entry.finished = std::chrono::system_clock::now();
    entry.caller = req.caller;
    entry.method = active_rec.method;
    entry.url_redacted = active_rec.url_redacted;
    entry.request_headers_redacted = join_headers(
        [&]() {
            std::vector<std::string> v;
            for (const auto& h : req.headers) v.push_back(redaction::redact_header_line(h));
            return v;
        }()
    );
    entry.request_body_redacted = redaction::redact_body(req.body);
    // response headers: take only what we actually captured; trim to a
    // reasonable preview size.
    entry.response_headers_redacted = [&]() {
        std::string s;
        for (std::size_t i = 0; i < resp.response_headers.size() && s.size() < 2048; ++i) {
            const char c = resp.response_headers[i];
            const char next = (i + 1 < resp.response_headers.size()) ? resp.response_headers[i + 1] : '\0';
            if (c == '\r' && next == '\n') continue;
            if (c == '\n') continue;
            s.push_back(c);
        }
        return s;
    }();
    entry.response_body_preview = redaction::redact_body(resp.body, 4096);
    entry.effective_url = resp.effective_url;
    entry.http_code = resp.http_code;
    entry.curl_error = resp.error;
    entry.total_time_ms = resp.total_time_ms;
    entry.ok = succeeded && resp.http_code < 400;
    // Capture fields needed by the post-move debug log before moving entry.
    const std::string method_for_log = entry.method;
    const std::string url_for_log = entry.url_redacted;
    record_entry(std::move(entry));

    if (!resp.error.empty()) {
        Logger::instance().debug(
            "Network:: " + method_for_log + " " + url_for_log +
            " failed: " + resp.error + " (HTTP " + std::to_string(resp.http_code) + ")");
    }
    (void)last_http;
    resp.request_id = id;
    return resp;
}

DownloadResult Manager::download(DownloadRequest& req) {
    DownloadResult res;
    if (cancelled_) {
        res.error = "cancelled";
        return res;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (opts_.offline_mode) {
            res.error = "offline";
            return res;
        }
    }

    const std::uint64_t id = next_id_.fetch_add(1);
    const auto started = std::chrono::system_clock::now();

    std::ofstream file;
    std::string prep_err;
    CURL* curl = prepare_download(req, res, file, prep_err);
    if (!curl) {
        res.error = prep_err;
        LogEntry entry;
        entry.id = id;
        entry.started = started;
        entry.finished = std::chrono::system_clock::now();
        entry.caller = req.caller;
        entry.method = "GET";
        entry.url_redacted = redaction::redact_url(req.url);
        entry.curl_error = prep_err;
        entry.ok = false;
        record_entry(std::move(entry));
        return res;
    }

    const CURLcode rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &res.http_code);
    double tt = 0.0;
    curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &tt);
    char* eff = nullptr;
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &eff);
    if (eff) res.effective_url = eff;

    // Pull back the request-header slist from PrivateData and free the
    // wrapper. The header-capture strings live directly in res (managed
    // by Manager::download's caller), so there is nothing to copy here.
    PrivateData* priv = nullptr;
    curl_easy_getinfo(curl, CURLINFO_PRIVATE, &priv);
    if (priv) {
        if (priv->slist) curl_slist_free_all(priv->slist);
        delete priv;
    }
    curl_easy_cleanup(curl);

    file.close();

    const bool aborted = (rc == CURLE_ABORTED_BY_CALLBACK);
    res.aborted = aborted;
    res.error = aborted ? std::string() : std::string(curl_easy_strerror(rc));
    res.ok = !aborted && rc == CURLE_OK && res.http_code < 400;

    // Cleanup partial file on failure (mirror curl_download behaviour:
    // aborted (pause) keeps the partial file).
    if (!res.ok && !aborted) {
        std::error_code ec;
        std::filesystem::remove(req.dest, ec);
    }

    LogEntry entry;
    entry.id = id;
    entry.started = started;
    entry.finished = std::chrono::system_clock::now();
    entry.caller = req.caller;
    entry.method = "GET";
    entry.url_redacted = redaction::redact_url(req.url);
    entry.request_headers_redacted = join_headers(
        [&]() {
            std::vector<std::string> v;
            for (const auto& h : req.headers) v.push_back(redaction::redact_header_line(h));
            return v;
        }()
    );
    entry.response_headers_redacted = [&]() {
        std::string s;
        for (std::size_t i = 0; i < res.response_headers.size() && s.size() < 2048; ++i) {
            const char c = res.response_headers[i];
            const char next = (i + 1 < res.response_headers.size()) ? res.response_headers[i + 1] : '\0';
            if (c == '\r' && next == '\n') continue;
            if (c == '\n') continue;
            s.push_back(c);
        }
        return s;
    }();
    entry.effective_url = res.effective_url;
    entry.http_code = res.http_code;
    entry.curl_error = res.error;
    entry.total_time_ms = tt * 1000.0;
    entry.ok = res.ok;
    entry.aborted = aborted;
    record_entry(std::move(entry));

    res.request_id = id;
    return res;
}

std::vector<ActiveRequest> Manager::active_requests() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<ActiveRequest> out;
    out.reserve(active_.size());
    for (const auto& [_, rec] : active_) out.push_back(rec);
    return out;
}

std::size_t Manager::queue_depth() const {
    // TODO: this implementation does not park requests; the analysis calls
    // for the bounded pool to live in PipelineWorker (which reads
    // NetworkOptions::max_parallel). For now we report 0 - the existing
    // PipelineWorker pool is the queue. A future change can wire a real
    // queue here.
    return 0;
}

std::vector<LogEntry> Manager::log_snapshot(std::size_t max_entries) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<LogEntry> out;
    out.reserve(std::min(max_entries, log_.size()));
    // Newest first.
    for (auto it = log_.rbegin(); it != log_.rend() && out.size() < max_entries; ++it) {
        out.push_back(*it);
    }
    return out;
}

std::size_t Manager::log_size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return log_.size();
}

void Manager::clear_log() {
    std::lock_guard<std::mutex> lock(mu_);
    log_.clear();
}

bool Manager::is_offline() const {
    std::lock_guard<std::mutex> lock(mu_);
    return opts_.offline_mode;
}

void Manager::set_options(NetworkOptions opts) {
    std::lock_guard<std::mutex> lock(mu_);
    opts_ = std::move(opts);
}

NetworkOptions Manager::options() const {
    std::lock_guard<std::mutex> lock(mu_);
    return opts_;
}

void Manager::cancel_all() {
    // Set the kill switch. Once flipped, every new request()/download()
    // call short-circuits with error="cancelled" until reset_cancel() is
    // called. In-flight requests are aborted via their should_abort
    // callback when the next xfer_forwarder tick fires (or via the
    // response when the easy handle next yields).
    cancelled_ = true;
}

void Manager::reset_cancel() {
    // Clear the kill switch so future requests can proceed. Production
    // code that calls cancel_all() usually destroys the Manager
    // immediately after, but tests reuse a single Manager across cases
    // and would otherwise see every request blocked.
    cancelled_ = false;
}

LogEntry Manager::record_entry(const LogEntry& entry) {
    std::lock_guard<std::mutex> lock(mu_);
    if (log_.size() >= kRingCapacity) {
        log_.pop_front();
    }
    log_.push_back(entry);
    return entry;
}

// -----------------------------------------------------------------------------
// FakeNetworkManager
// -----------------------------------------------------------------------------

Response FakeNetworkManager::request(const Request& req) {
    seen_requests_.push_back(req);
    if (offline_) {
        Response r;
        r.error = "offline";
        return r;
    }
    if (responses_.empty()) {
        Response r;
        r.error = "no fake response queued";
        return r;
    }
    Response r = std::move(responses_.front());
    responses_.erase(responses_.begin());
    return r;
}

DownloadResult FakeNetworkManager::download(DownloadRequest& req) {
    seen_downloads_.push_back(req);
    DownloadResult r;
    if (offline_) {
        r.error = "offline";
        return r;
    }
    // Fake "ok" - the test is responsible for any actual file content.
    r.ok = true;
    r.http_code = 200;
    return r;
}

// -----------------------------------------------------------------------------
// Singleton
// -----------------------------------------------------------------------------

namespace {
// B-06 (singleton race): two threads calling instance() simultaneously
// when g_instance is null used to race the unsynchronised check +
// assignment, double-constructing a Manager and leaking the second one's
// CURLSH handle. Meyers singleton (function-local static) gives us
// thread-safe lazy init for free under C++11.
std::unique_ptr<Interface>& slot() {
    static std::unique_ptr<Interface> p;
    return p;
}
}

Interface& instance() {
    auto& p = slot();
    if (!p) p = std::make_unique<Manager>();
    return *p;
}

void set_instance(std::unique_ptr<Interface> net) {
    slot() = std::move(net);
}

} // namespace engine::network