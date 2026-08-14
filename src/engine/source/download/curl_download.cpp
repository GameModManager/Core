#include "engine/source/download/curl_download.h"

#include "engine/core/log/logger.h"
#include "engine/source/nexus_http.h"  // encode_url_path (shared URL helper)

#include <curl/curl.h>

#include <cctype>
#include <fstream>
#include <sstream>

namespace engine::download {

namespace {

int xferinfo_callback(void* user_data, curl_off_t dltotal, curl_off_t dlnow,
                      curl_off_t ultotal, curl_off_t ulnow) {
    (void)ultotal; (void)ulnow;
    auto* dp = static_cast<Progress*>(user_data);
    if (dp && dp->should_abort && dp->should_abort()) {
        // Pause requested - abort this transfer; the partial file is kept.
        return 1;
    }
    if (dp && dp->callback) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration<double>(now - dp->start).count();
        double speed = (elapsed > 0.001) ? (dlnow / elapsed) : 0.0;
        // During a resumed transfer curl reports only the remaining bytes
        // (dlnow from 0, dltotal = remainder), so add the resume base back to
        // keep the UI progress continuous across pause/resume.
        dp->callback(dp->resume_base + dlnow, dp->resume_base + dltotal, speed);
    }
    return 0;
}

size_t write_to_file(void* ptr, size_t size, size_t nmemb, void* stream) {
    auto* file = static_cast<std::ofstream*>(stream);
    if (!file || !file->is_open()) return 0;
    auto written = static_cast<std::streamsize>(size * nmemb);
    file->write(static_cast<const char*>(ptr), written);
    return file->good() ? (size * nmemb) : 0;
}

bool starts_with_ci(const std::string& line, const std::string& prefix) {
    if (line.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(line[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i])))
            return false;
    }
    return true;
}

std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

} // namespace

size_t capture_content_disposition(void* ptr, size_t size, size_t nmemb,
                                   void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    const std::size_t len = size * nmemb;
    std::string line(static_cast<const char*>(ptr), len);
    // Strip the trailing CRLF libcurl appends to header lines.
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        line.pop_back();

    if (!starts_with_ci(line, "Content-Disposition:")) return len;
    // The last hop's value wins: a redirect hop may set one that the final
    // response then overrides.
    *out = trim(line.substr(std::string("Content-Disposition:").size()));
    return len;
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
    std::string lower = header_value;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

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
    auto* curl = curl_easy_init();
    if (!curl) return false;

    std::ofstream file;
    if (resume_from > 0)
        file.open(dest_path, std::ios::binary | std::ios::app);
    else
        file.open(dest_path, std::ios::binary);
    if (!file) {
        curl_easy_cleanup(curl);
        return false;
    }

    const std::string encoded_url = engine::encode_url_path(url);
    curl_easy_setopt(curl, CURLOPT_URL, encoded_url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                     opts.user_agent.empty()
                         ? "GameModManager/0.1 (Nexus Provider)"
                         : opts.user_agent.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    if (opts.long_lived)
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    else
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &file);

    if (!opts.cookie_header.empty())
        curl_easy_setopt(curl, CURLOPT_COOKIE, opts.cookie_header.c_str());

    if (opts.content_disposition) {
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, capture_content_disposition);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, opts.content_disposition);
    }

    if (resume_from > 0)
        curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE,
                         static_cast<curl_off_t>(resume_from));

    if (progress && progress->callback) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferinfo_callback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, progress);
    }

    CURLcode res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (opts.effective_url) {
        char* eff = nullptr;
        curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &eff);
        if (eff) *opts.effective_url = eff;
    }
    curl_easy_cleanup(curl);

    file.close();

    bool was_aborted = (res == CURLE_ABORTED_BY_CALLBACK);
    if (aborted) *aborted = was_aborted;

    if (res != CURLE_OK || http_code >= 400) {
        if (!was_aborted) {
            Logger::instance().error(
                "curl_download error: " + std::string(curl_easy_strerror(res)) +
                " (code=" + std::to_string(res) +
                ", http_code=" + std::to_string(http_code) + ")");
            std::error_code ec;
            std::filesystem::remove(dest_path, ec);
        }
        return false;
    }
    return true;
}

} // namespace engine::download
