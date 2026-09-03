#pragma once

// =============================================================================
// Network:: - the single gateway for every inbound/outbound network connection
// =============================================================================
//
// OWNERSHIP (GUARDRAIL):
//   This header is the ONE sanctioned entry point for internet I/O in the
//   engine. Every raw `curl_easy_init`, `QNetworkAccessManager`, or any other
//   libcurl direct call MUST live behind this facade.
//
//   Migration rule (post-network::):
//     - providers call Network::instance().request(...) / .download(...)
//     - if you need to add a new call site, do NOT include <curl/curl.h>
//       outside this directory
//
//   Documented exception: src/ui/modinfo/description_browser.cpp still owns a
//   single QNAM instance for QTextDocument image integration (loadResource()
//   is a synchronous callback QNAM answers). It is a UI-only island, does not
//   touch the engine, and is small enough that routing through Network::
//   would not pay off. Keep it that way.
//
//   CI / lint suggestion: the following grep should remain EMPTY in CI:
//     rg -n 'curl_easy_init|curl_easy_perform' projects/Core/src --type cpp \
//        -g '!src/engine/network/**' -g '!src/engine/source/download/curl_download.cpp' \
//        -g '!src/engine/source/nexus_http.cpp'
//     rg -n 'QNetworkAccessManager' projects/Core/src --type cpp \
//        -g '!src/ui/modinfo/description_browser.cpp'
//
// REDACTION:
//   apikey / Authorization / Cookie / token values are NEVER logged. The
//   central redact() pass strips them at log time so caller code does not
//   need to remember to scrub. Body redaction handles JSON keys too.
//
// THREADING:
//   Manager::request() and download() block the calling thread (libcurl easy
//   interface). They are safe to call from any thread. The log ring buffer
//   and the in-flight map use an internal mutex.
//
// OPTIONS:
//   NetworkOptions is plain-data, injected via set_options(). The engine never
//   reaches into QSettings; the UI builds a NetworkOptions from Settings on
//   startup and on changes and pushes it in.

// Forward declaration only. Including <curl/curl.h> here would leak libcurl
// types into every translation unit that depends on network_manager.h.
// prepare_request/prepare_download are private helpers so callers never see
// the underlying handle. When this header is included by the
// implementation file (which does pull the real libcurl headers), the
// typedefs already exist - skip them to avoid a redefinition error.
// CURLINC_CURL_H is libcurl's own include guard (set in <curl/curl.h>).
#if !defined(CURLINC_CURL_H)
typedef struct CURL CURL;
typedef struct curl_slist curl_slist;
typedef struct CURLSH CURLSH;
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::network {

// NET_CALLER expands to a short "<symbol> @ <file>:<line>" string identifying
// where the request originated. The Debug panel's Network tab shows this
// verbatim, so callers always have a hop from "request failed" -> "the mod
// list refresh did it" -> "nexus_provider.cpp:252".
#define NET_CALLER \
    (std::string(__func__) + " @ " + __FILE__ + ":" + std::to_string(__LINE__))

// -----------------------------------------------------------------------------
// Redaction - central scrubbing for secrets that MUST NEVER hit the log ring.
// Returns "<key>: <redacted>" when the key is sensitive (apikey, Authorization,
// Cookie, Set-Cookie, token, ...). Headers/URLs/bodies are all run through
// these helpers before they are written to the log.
// -----------------------------------------------------------------------------
namespace redaction {

// Returns true for header names whose value should be hidden (apikey,
// authorization, cookie, set-cookie, token, x-api-key, ...). Case-insensitive.
bool is_sensitive_header(const std::string& header_name);

// Scrub a "Header-Name: value" pair. Returns either the input unchanged
// (when not sensitive) or "Header-Name: <redacted>".
std::string redact_header_line(const std::string& line);

// Scrub a URL: any query parameter whose key matches a sensitive token is
// replaced with "<redacted>" so cookies / apikeys pasted into the query
// string do not leak.
std::string redact_url(const std::string& url);

// Scrub a JSON-ish body: keys named apikey / token / password / authorization
// are replaced with "<redacted>" in-place (string-preserving). Best-effort:
// malformed JSON is returned unchanged.
std::string redact_body(const std::string& body, std::size_t preview_bytes = 4096);

// Return a short placeholder for a cookie blob: "<name>=<redacted>, ... (N
// chars)". Keeps the format human-readable for debugging without leaking
// values.
std::string redact_cookie(const std::string& cookie);

} // namespace redaction

// -----------------------------------------------------------------------------
// Public data model
// -----------------------------------------------------------------------------

enum class Method { Get, Post, Delete, Put, Patch, Head };

inline const char* method_name(Method m) {
    switch (m) {
    case Method::Get:    return "GET";
    case Method::Post:   return "POST";
    case Method::Delete: return "DELETE";
    case Method::Put:    return "PUT";
    case Method::Patch:  return "PATCH";
    case Method::Head:   return "HEAD";
    }
    return "GET";
}

// HTTP headers as a flat list of "Name: value" lines (curl_slist-compatible).
// Stored as std::vector<std::string> so the engine layer stays Qt-free.
using Headers = std::vector<std::string>;

// Options struct, injected from the UI. Plain-data so the engine does not
// reach into QSettings. All fields have safe defaults.
struct NetworkOptions {
    bool offline_mode = false;
    bool use_proxy = false;
    std::string proxy_host;          // e.g. "127.0.0.1"
    int proxy_port = 8080;
    // Max concurrent downloads (Network:: download paths + PipelineWorker).
    // 0 = legacy 2-slot behaviour (matches the previous PipelineWorker
    // kMaxConcurrentDownloads default).
    int max_parallel = 2;
    // Nexus: serialize downloads even when a parallel slot is free (free /
    // supporter tier). Premium: parallel. Defaults ON to match the
    // pre-Network:: behaviour.
    bool nexus_queue_downloads = true;
    // Workshop hourly rate limit (mirrors Settings).
    int workshop_rate_limit_per_hour = 60;
    // Default per-request timeout for non-download requests (Nexus JSON,
    // probes, etc). Downloads use 0 (no overall cap) when long_lived.
    int default_timeout_seconds = 30;
    // Max retries on transient failure (5xx, network). 0 = no retry.
    int max_retries = 0;
    // Initial backoff in milliseconds (doubles each attempt up to 8x).
    int retry_backoff_ms = 500;
};

// In-memory request (GET/POST for APIs, JSON fetches, probes).
struct Request {
    Method method = Method::Get;
    std::string url;
    Headers headers;            // "Name: value" lines, appended to defaults
    std::string body;           // POST/PUT/PATCH body (empty = no body)
    std::string caller;         // set automatically via NET_CALLER
    std::chrono::seconds timeout{30};
    bool follow_redirect = true;
    // Cap response bytes; 0 = no cap. Used by LoversLab scrape to bound
    // misconfigured servers.
    std::int64_t max_bytes = 0;
};

struct Response {
    long http_code = 0;
    std::string body;
    std::string response_headers; // raw "Name: value\r\n" lines
    std::string effective_url;
    std::string content_disposition;
    std::string error;            // libcurl error string on failure
    double total_time_ms = 0.0;
    std::uint64_t request_id = 0; // matches LogEntry::id for cross-reference
};

// File download (with resume, progress callback, queue + parallel limit).
struct ProgressCb {
    std::function<void(std::int64_t downloaded, std::int64_t total, double bps)> on;
    std::function<bool()> should_abort;  // returns true to abort (pause)
    std::int64_t resume_base = 0;
};

struct DownloadRequest {
    std::string url;
    std::filesystem::path dest;
    Headers headers;
    std::string caller;
    ProgressCb progress;
    // Long-lived (e.g. large archive): no overall timeout, only connect
    // timeout. Matches the pre-Network:: curl_download long_lived flag.
    bool long_lived = false;
    std::int64_t resume_from = 0;
};

struct DownloadResult {
    bool ok = false;
    std::string error;
    long http_code = 0;
    std::uint64_t request_id = 0;
    bool aborted = false;             // pause: partial file kept
    std::int64_t bytes_downloaded = 0;
    std::int64_t bytes_total = 0;
    // Final URL after redirects. Empty if no redirect happened.
    std::string effective_url;
    // Server-provided Content-Disposition value, if any (LoversLab uses
    // this to name downloaded archives when the URL is opaque).
    std::string content_disposition;
    // Raw response headers, written through the same single-header pass that
    // also extracts the Content-Disposition above. Lives in DownloadResult
    // so its address is stable for the entire download lifetime; a
    // previous stack-local version dangled once prepare_download
    // returned and corrupted the destination string.
    std::string response_headers;
};

// One ring-buffer entry per request (active + finished). Logged to file and
// shown in the Debug panel Network tab.
struct LogEntry {
    std::uint64_t id = 0;
    std::chrono::system_clock::time_point started{};
    std::chrono::system_clock::time_point finished{};
    std::string caller;                          // NET_CALLER string
    std::string method;                          // "GET", "POST", ...
    std::string url_redacted;
    std::string request_headers_redacted;
    std::string request_body_redacted;
    std::string response_headers_redacted;
    std::string response_body_preview;           // first 4 KiB
    std::string effective_url;
    long http_code = 0;
    std::string curl_error;
    std::int64_t bytes_downloaded = 0;
    std::int64_t bytes_total = 0;
    double total_time_ms = 0.0;
    bool ok = false;
    bool aborted = false;                        // pause / should_abort
};

// Live snapshot of an in-flight request for the Debug panel.
struct ActiveRequest {
    std::uint64_t id = 0;
    std::string caller;
    std::string method;
    std::string url_redacted;
    std::chrono::system_clock::time_point started{};
};

// -----------------------------------------------------------------------------
// Interface - the network gateway contract. Every consumer goes through this,
// not libcurl directly. Tests use FakeNetworkManager to avoid the wire.
// -----------------------------------------------------------------------------

class Interface {
public:
    virtual ~Interface() = default;

    // In-memory request (GET/POST). Blocking; returns a Response with the
    // body and headers. Never throws.
    virtual Response request(const Request& req) = 0;

    // File download with resume + progress + abort. Blocking. Returns
    // DownloadResult; on pause (aborted=true) the partial file is kept on
    // disk for a later resume.
    virtual DownloadResult download(DownloadRequest& req) = 0;

    // Live in-flight requests (for the Debug panel "Network" tab).
    virtual std::vector<ActiveRequest> active_requests() const = 0;

    // Pending queued downloads (those that the manager parked because
    // max_parallel slots were full). Empty for in-memory request() calls.
    virtual std::size_t queue_depth() const = 0;

    // Recent log entries, newest first.
    virtual std::vector<LogEntry> log_snapshot(std::size_t max_entries = 500) const = 0;

    // Drop the log ring buffer (e.g. when switching instances).
    virtual void clear_log() = 0;

    // True while offline_mode is on (NetworkOptions::offline_mode). request
    // and download return immediately with error="offline" when set.
    virtual bool is_offline() const = 0;

    // Replace the current options. Triggers immediate re-apply on the next
    // request.
    virtual void set_options(NetworkOptions opts) = 0;
    virtual NetworkOptions options() const = 0;

    // Cancel every in-flight and queued request. After this returns, all
    // NEW request()/download() calls also short-circuit with
    // error="cancelled" until reset_cancel() is called. Used for clean
    // shutdown (typically paired with reset_cancel() in tests, or
    // followed by destroying the Manager entirely in production).
    virtual void cancel_all() = 0;

    // Clears the cancel flag set by cancel_all() so future requests can
    // proceed. Mostly useful for tests and for re-using a Manager across
    // shutdown cycles - production code that intends to discard the
    // Manager can simply destroy it instead.
    virtual void reset_cancel() = 0;
};

// -----------------------------------------------------------------------------
// Manager - the concrete libcurl-backed implementation. One process-wide
// instance is enough; tests construct their own.
// -----------------------------------------------------------------------------

class Manager : public Interface {
public:
    Manager();
    ~Manager() override;

    Manager(const Manager&) = delete;
    Manager& operator=(const Manager&) = delete;

    Response request(const Request& req) override;
    DownloadResult download(DownloadRequest& req) override;

    std::vector<ActiveRequest> active_requests() const override;
    std::size_t queue_depth() const override;
    std::vector<LogEntry> log_snapshot(std::size_t max_entries = 500) const override;
    void clear_log() override;
    bool is_offline() const override;
    void set_options(NetworkOptions opts) override;
    NetworkOptions options() const override;
    void cancel_all() override;
    void reset_cancel() override;

    // For unit tests: number of ring slots filled. Equivalent to
    // log_snapshot().size() but cheaper.
    std::size_t log_size() const;

private:
    // Build + configure a CURL* easy handle from a Request (in-memory path).
    // Caller owns the returned CURL* and must call curl_easy_cleanup.
    // Returns nullptr on failure (writes the error into out_err).
    CURL* prepare_request(const Request& req, Response& out_resp,
                          std::string& out_err);

    // Build + configure a CURL* easy handle for a file download.
    CURL* prepare_download(DownloadRequest& req, DownloadResult& out_res,
                           std::ofstream& out_file, std::string& out_err);

    // Record an entry into the ring buffer + emit a debug log line. Caller
    // supplies everything except id (auto-assigned) and timestamps (filled
    // in here).
    LogEntry record_entry(const LogEntry& entry);

    // Apply NetworkOptions to a CURL* handle (proxy, user agent, etc).
    void apply_options(CURL* curl) const;

    mutable std::mutex mu_;

    // Active in-flight tracking (request_id -> snapshot).
    std::unordered_map<std::uint64_t, ActiveRequest> active_;

    // Bounded ring (oldest dropped when over capacity). 2000 entries by
    // default - matches the design goal.
    static constexpr std::size_t kRingCapacity = 2000;
    std::deque<LogEntry> log_;

    std::atomic<std::uint64_t> next_id_{1};
    NetworkOptions opts_;

    // libcurl share handle: keeps DNS cache, TLS session cache, and
    // connection pool across easy handles. Created once per Manager.
    CURLSH* share_ = nullptr;

    // Per-data-kind mutexes for the share handle's lock callbacks (DNS,
    // SSL session, connection). curl_share_setopt only stores a single
    // userdata pointer, so we point it at this map of mutexes.
    mutable std::unordered_map<int, std::mutex> share_locks_;

    // H2: cancel_all() previously flipped an atomic<bool> and never reset
    // it, which permanently blocked every subsequent request() /
    // download() (every caller received error="cancelled"). The flag is
    // now paired with reset_cancel() and the doc comment makes the
    // session-scope kill-switch nature explicit.
    std::atomic<bool> cancelled_{false};
};

// -----------------------------------------------------------------------------
// FakeNetworkManager - canned responses for unit tests. Never touches the
// network. Records every Request so tests can assert on call sites.
// -----------------------------------------------------------------------------

class FakeNetworkManager : public Interface {
public:
    FakeNetworkManager() = default;
    ~FakeNetworkManager() override = default;

    Response request(const Request& req) override;
    DownloadResult download(DownloadRequest& req) override;

    std::vector<ActiveRequest> active_requests() const override { return {}; }
    std::size_t queue_depth() const override { return 0; }
    std::vector<LogEntry> log_snapshot(std::size_t = 500) const override { return {}; }
    void clear_log() override {}
    bool is_offline() const override { return offline_; }
    void set_options(NetworkOptions opts) override {
        offline_ = opts.offline_mode;
        opts_ = std::move(opts);
    }
    NetworkOptions options() const override { return opts_; }
    void cancel_all() override {}
    void reset_cancel() override {}

    // Configure the next response that request() will return. Popped FIFO.
    void enqueue_response(Response r) { responses_.push_back(std::move(r)); }

    // Record of every request handed to request()/download() since the
    // fake was constructed (or since clear_calls()).
    const std::vector<Request>& seen_requests() const { return seen_requests_; }
    const std::vector<DownloadRequest>& seen_downloads() const { return seen_downloads_; }
    void clear_calls() { seen_requests_.clear(); seen_downloads_.clear(); }

    // Toggle: when true, request()/download() short-circuit with
    // error="offline" regardless of queued responses.
    void set_offline(bool on) { offline_ = on; }

private:
    std::vector<Response> responses_;
    std::vector<Request> seen_requests_;
    std::vector<DownloadRequest> seen_downloads_;
    NetworkOptions opts_;
    bool offline_ = false;
};

// -----------------------------------------------------------------------------
// Process-wide singleton accessor. The real Manager is constructed on first
// use; tests call set_instance() to inject a FakeNetworkManager.
// -----------------------------------------------------------------------------

Interface& instance();
void set_instance(std::unique_ptr<Interface> net);

} // namespace engine::network