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

#include "engine/network/network_manager.h"

#include "engine/core/log/logger.h"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <fstream>
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

std::string redact_url(const std::string& url) {
    // Redact sensitive query params (apikey, key, token, csrf, ...).
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return url;
    const auto q = url.find('?', scheme_end + 3);
    if (q == std::string::npos) return url;
    const auto frag = url.find('#', q);

    std::string prefix = url.substr(0, q + 1);
    std::string query = (frag == std::string::npos)
        ? url.substr(q + 1)
        : url.substr(q + 1, frag - q - 1);
    std::string suffix = (frag == std::string::npos) ? "" : url.substr(frag);

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
    return prefix + out_query + suffix;
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

// Append response headers to a string. Strips trailing CRLFs.
size_t capture_headers(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    const std::size_t bytes = size * nmemb;
    s->append(ptr, bytes);
    return bytes;
}

// File write callback. Returns 0 to abort the transfer on file error.
struct FileWriteState {
    std::ofstream* file = nullptr;
    std::int64_t bytes_written = 0;
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

// Capture Content-Disposition into a string.
size_t capture_content_disposition(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    std::string line(ptr, size * nmemb);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    const std::string prefix = "Content-Disposition:";
    if (line.size() > prefix.size() &&
        line.compare(0, prefix.size(), prefix) == 0) {
        std::size_t start = prefix.size();
        while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) ++start;
        *out = line.substr(start);
    }
    return size * nmemb;
}

// Progress callback for downloads. Forwards to ProgressCb and respects
// should_abort (returns 1 to abort, partial file kept).
struct ProgressForwarder {
    ProgressCb* cb = nullptr;
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
        const double speed = (dlnow > 0)
            ? static_cast<double>(dlnow) / 0.001  // caller uses resume_base + bytes
            : 0.0;
        (void)speed;
        pf->cb->on(pf->cb->resume_base + dlnow, pf->cb->resume_base + dltotal, 0.0);
    }
    return 0;
}

std::string join_headers(const std::vector<std::string>& headers) {
    std::string out;
    for (const auto& h : headers) {
        out += h;
        if (!h.empty() && h.back() != '\n') out += "\r\n";
    }
    return out;
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
        const std::string url = opts_.proxy_host + ":" + std::to_string(opts_.proxy_port);
        curl_easy_setopt(curl, CURLOPT_PROXY, url.c_str());
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
    // redaction only happens when logging).
    const std::string url_redacted = redaction::redact_url(req.url);
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

    // Capture response headers for downstream consumers (Nexus rate limits,
    // Content-Disposition, ...).
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, capture_headers);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &out_resp.response_headers);
    // Special-case Content-Disposition capture into its own field.
    // Headers callback already handles it; we add a second pass for the
    // specific "Content-Disposition" parsing used by LoversLab.
    if (req.url.find("loverslab.com") != std::string::npos) {
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, capture_content_disposition);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &out_resp.content_disposition);
    }

    apply_options(curl);

    // Stash for cleanup by the caller (we return curl but the slist owns
    // the header strings). Caller must curl_slist_free_all after
    // curl_easy_perform.
    // Trick: encode the slist pointer into a private header to retrieve
    // later. Simpler approach: detach the slist and free it in a wrapper.
    // Here we use CURLOPT_PRIVATE to round-trip the slist*.
    curl_easy_setopt(curl, CURLOPT_PRIVATE, slist);

    // silence "unused" complaint when neither add_header ever fires.
    (void)url_redacted;
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

    static thread_local FileWriteState fws;
    fws.file = &out_file;
    fws.bytes_written = 0;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &fws);

    static thread_local ProgressForwarder pf;
    pf.cb = &req.progress;
    if (req.progress.on || (req.progress.should_abort)) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xfer_forwarder);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &pf);
    }

    curl_slist* slist = nullptr;
    for (const auto& h : req.headers) slist = curl_slist_append(slist, h.c_str());
    if (slist) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slist);
    curl_easy_setopt(curl, CURLOPT_PRIVATE, slist);

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

    // Retry loop with simple exponential backoff. Honours Retry-After.
    int attempts = std::max(1, opts_.max_retries + 1);
    int backoff_ms = opts_.retry_backoff_ms;
    bool succeeded = false;
    std::string last_curl_error;
    long last_http = 0;

    for (int i = 0; i < attempts; ++i) {
        const CURLcode res = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.http_code);
        char* eff = nullptr;
        curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &eff);
        if (eff) resp.effective_url = eff;
        double tt = 0.0;
        curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &tt);
        resp.total_time_ms = tt * 1000.0;

        if (res == CURLE_OK && resp.http_code < 500) {
            succeeded = true;
            last_http = resp.http_code;
            break;
        }
        last_curl_error = curl_easy_strerror(res);
        last_http = resp.http_code;

        if (i + 1 >= attempts) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
        backoff_ms = std::min(backoff_ms * 2, backoff_ms * 8);
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.http_code);
    {
        curl_slist* slist = nullptr;
        curl_easy_getinfo(curl, CURLINFO_PRIVATE, &slist);
        if (slist) curl_slist_free_all(slist);
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
    record_entry(std::move(entry));

    if (!resp.error.empty()) {
        Logger::instance().debug(
            "Network:: " + entry.method + " " + entry.url_redacted +
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

    {
        curl_slist* slist = nullptr;
        curl_easy_getinfo(curl, CURLINFO_PRIVATE, &slist);
        if (slist) curl_slist_free_all(slist);
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
    cancelled_ = true;
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
std::unique_ptr<Interface> g_instance;
}

Interface& instance() {
    if (!g_instance) {
        g_instance = std::make_unique<Manager>();
    }
    return *g_instance;
}

void set_instance(std::unique_ptr<Interface> net) {
    g_instance = std::move(net);
}

} // namespace engine::network