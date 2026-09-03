// =============================================================================
// engine::curl_download - thin wrapper around Network::
// -----------------------------------------------------------------------------
// Preserved for the existing call sites (Nexus, LoversLab, self-updater, LOOT
// masterlists, icon fetcher, instance masterlist cache). Internally it now
// delegates to the centralized Network:: gateway so every byte that hits the
// wire shows up in the Debug panel Network tab. Behavior is intentionally
// unchanged: same Options / Progress / resume / pause semantics, same return
// shape (ok / http_code / aborted / partial file kept on abort).
//
// Once every consumer has been ported to call Network:: directly this header
// stays in place as the only sanctioned way for new code to talk to the wire
// without owning libcurl.
// =============================================================================

#include "engine/source/download/curl_download.h"

#include "engine/core/log/logger.h"
#include "engine/network/network_manager.h"
#include "engine/source/http_util.h"  // encode_url_path (shared URL helper)

#include <chrono>
#include <filesystem>

namespace engine::Source::DownloadManager {

namespace {

// Translate a libcurl-ish "Options" struct into a Network:: DownloadRequest.
network::DownloadRequest to_request(const std::string& url,
                                   const std::filesystem::path& dest_path,
                                   const Options& opts,
                                   Progress* progress,
                                   int64_t resume_from,
                                   const std::string& caller) {
    network::DownloadRequest r;
    // The original helper URL-encodes the path component, so we mirror that
    // before handing it to libcurl - same behaviour as the pre-Network:: code.
    r.url = engine::Source::Http::encode_url_path(url);
    r.dest = dest_path;
    r.caller = caller;
    r.long_lived = opts.long_lived;
    r.resume_from = resume_from;
    if (!opts.cookie_header.empty())
        r.headers.push_back("Cookie: " + opts.cookie_header);
    if (!opts.user_agent.empty())
        r.headers.push_back("User-Agent: " + opts.user_agent);
    if (progress) {
        r.progress.on = progress->callback;
        r.progress.should_abort = progress->should_abort;
        r.progress.resume_base = progress->resume_base;
    }
    return r;
}

} // namespace

size_t capture_content_disposition(void* ptr, size_t size, size_t nmemb,
                                   void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    std::string line(static_cast<const char*>(ptr), size * nmemb);
    // Strip the trailing CRLF libcurl appends to header lines.
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        line.pop_back();

    // Case-insensitive prefix check: the original code matched the
    // title-cased form only, which dropped the header for any server
    // that sent lowercase or mixed-case. Network:: now captures
    // Content-Disposition itself, so this helper is dead in the main
    // path - but keep the old contract intact in case external callers
    // (or a future reintroduction) use it.
    constexpr std::string_view prefix = "Content-Disposition:";
    if (line.size() <= prefix.size()) return size * nmemb;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        char a = line[i];
        char b = prefix[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b + ('a' - 'A'));
        if (a != b) return size * nmemb;
    }
    // The last hop's value wins: a redirect hop may set one that the final
    // response then overrides.
    std::size_t start = prefix.size();
    while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) ++start;
    *out = line.substr(start);
    return size * nmemb;
}

std::string percent_decode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            int h = hex(in[i + 1]), l = hex(in[i + 2]);
            if (h >= 0 && l >= 0) {
                out.push_back(static_cast<char>((h << 4) | l));
                i += 2;
                continue;
            }
        }
        out.push_back(in[i]);
    }
    return out;
}

std::string parse_content_disposition_filename(const std::string& header_value) {
    // RFC 6266 / 5987 forms, matched case-insensitively:
    //   filename="foo.7z"
    //   filename=foo.7z
    //   filename*=UTF-8''foo%20bar.7z
    auto to_lower = [](std::string s) {
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    std::string lower = to_lower(header_value);

    const std::string rfc5987 = "filename*=";
    const std::string plain = "filename=";

    std::size_t pos = std::string::npos;
    bool encoded = false;
    if ((pos = lower.find(rfc5987)) != std::string::npos) {
        encoded = true;
    } else if ((pos = lower.find(plain)) != std::string::npos) {
        // "filename=" may also appear inside "filename*=..."; only accept a
        // standalone match that is not part of a filename*= occurrence.
        if (pos > 0 && lower[pos - 1] == '*') pos = std::string::npos;
    }
    if (pos == std::string::npos) return {};

    std::string value = header_value.substr(pos + (encoded ? rfc5987.size() : plain.size()));

    if (encoded) {
        // filename*=charset'lang'<pct-encoded value>
        const std::size_t first = value.find('\'');
        if (first != std::string::npos) {
            const std::size_t second = value.find('\'', first + 1);
            if (second != std::string::npos)
                value = value.substr(second + 1);
        }
        value = percent_decode(value);
    }

    // Trim.
    auto trim = [](std::string s) {
        std::size_t b = 0, e = s.size();
        while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
        while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
        return s.substr(b, e - b);
    };
    // Strip a trailing parameter list ("; size=...") unless quoted.
    value = trim(value);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    } else {
        const std::size_t semi = value.find(';');
        if (semi != std::string::npos) value = value.substr(0, semi);
        value = trim(value);
    }
    // Quoted value with the quote not at the end (e.g. filename="a"b): keep
    // everything inside the first pair of quotes.
    if (!value.empty() && value.front() == '"') {
        const std::size_t close = value.find('"', 1);
        if (close != std::string::npos)
            value = value.substr(1, close - 1);
    }

    // Drop any directory components.
    const std::size_t slash = value.find_last_of("/\\");
    if (slash != std::string::npos) value = value.substr(slash + 1);

    value = trim(value);
    if (value.empty() || value == "." || value == "..") return {};
    return value;
}

bool curl_download(const std::string& url,
                   const std::filesystem::path& dest_path,
                   long& http_code,
                   const Options& opts,
                   Progress* progress,
                   int64_t resume_from,
                   bool* aborted) {
    network::DownloadRequest req = to_request(url, dest_path, opts, progress,
                                              resume_from, NET_CALLER);

    auto res = network::instance().download(req);
    http_code = res.http_code;
    if (aborted) *aborted = res.aborted;

    if (!res.ok) {
        if (res.aborted) {
            // Pause requested - partial file is kept.
            return false;
        }
        // Network:: already removed the partial file on failure. We mirror
        // the original helper's behaviour by logging through Logger::error
        // so callers don't need to change their diagnostics.
        Logger::instance().error(
            "curl_download error: " + res.error +
            " (http_code=" + std::to_string(res.http_code) + ")");
        return false;
    }
    return true;
}

} // namespace engine::Source::DownloadManager