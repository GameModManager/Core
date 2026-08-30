#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace engine::Source {

// Download types and free functions, formerly in engine::download.
namespace DownloadManager {

// Progress reporting for a curl transfer (the UI's pause/progress plumbing).
struct Progress {
    std::function<void(int64_t, int64_t, double)> callback;
    std::function<bool()> should_abort;
    int64_t resume_base = 0;  // bytes already downloaded in a previous run
    std::chrono::steady_clock::time_point start;
};

// Download behavior that varies per provider. All fields optional.
struct Options {
    std::string cookie_header;        // raw "name=value; name2=value2" Cookie
                                      // header value (session-auth sites like
                                      // LoversLab). Sent on every request the
                                      // transfer makes, including redirects.
    std::string user_agent;           // empty = "GameModManager/0.1 (Nexus
                                      // Provider)" (historical default).
    std::string* content_disposition = nullptr;  // out: final response's
                                      // "Content-Disposition:" header value.
    std::string* effective_url = nullptr;        // out: final URL after redirects.
    bool long_lived = false;          // true = connect timeout only (no overall
                                      // transfer timeout), for large archives.
};

// Downloads url to dest_path. On success dest_path holds the complete file.
// On an abort (pause) the partial file is KEPT so a later run can resume via
// HTTP Range; on any other failure the partial file is removed. http_code
// receives the final HTTP status. Returns false on curl error / HTTP >= 400.
bool curl_download(const std::string& url,
                   const std::filesystem::path& dest_path,
                   long& http_code,
                   const Options& opts = {},
                   Progress* progress = nullptr,
                   int64_t resume_from = 0,
                   bool* aborted = nullptr);

// Extract the filename from a "Content-Disposition" header value, e.g.
//   attachment; filename="mod.7z"          -> "mod.7z"
//   attachment; filename=mod.7z            -> "mod.7z"
//   attachment; filename*=UTF-8''a%20b.7z  -> "a b.7z"
// Directory components are stripped. Empty when no usable filename is present.
std::string parse_content_disposition_filename(const std::string& header_value);

// Percent-decode %XX escapes (e.g. for RFC 5987 filename* values or URL path
// segments). Non-hex escapes are left as-is.
std::string percent_decode(const std::string& in);

// libcurl CURLOPT_HEADERFUNCTION-compatible callback: stores the last
// "Content-Disposition:" value seen into *(std::string*)userdata. Shared so
// header-only probes (resolve_download_info) capture it the same way as the
// real download.
size_t capture_content_disposition(void* ptr, size_t size, size_t nmemb,
                                   void* userdata);

} // namespace DownloadManager
} // namespace engine::Source
