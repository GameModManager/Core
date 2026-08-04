#include "engine/source/loverslab_provider.h"

#include "engine/download/curl_download.h"
#include "engine/loverslab_auth.h"
#include "engine/model/mod.h"
#include "engine/pipeline/pipeline.h"
#include "engine/log/logger.h"
#include "engine/source/nexus_http.h"  // encode_url_path

#include <curl/curl.h>

#include <chrono>
#include <cctype>
#include <filesystem>
#include <string>

namespace engine {

namespace {

// Discard the response body (we only want the headers). Returning 0 aborts
// the transfer once the body starts; headers were already delivered.
size_t discard_body(void*, size_t, size_t, void*) {
    return 0;
}

struct Probe {
    std::string content_disposition;
    std::string effective_url;
    long http_code = 0;
};

// Header-only GET: follow redirects, capture the final Content-Disposition and
// URL, abort before reading any body. Used to learn the real archive name
// (LoversLab serves .7z/.rar/.zip — the generic <id>.zip default would break
// extraction) and to sanity-check the session cookie.
Probe probe_download(const std::string& url, const std::string& cookie) {
    Probe p;
    auto* curl = curl_easy_init();
    if (!curl) return p;

    const std::string encoded = engine::encode_url_path(url);
    curl_easy_setopt(curl, CURLOPT_URL, encoded.c_str());
    if (!cookie.empty())
        curl_easy_setopt(curl, CURLOPT_COOKIE, cookie.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                     "GameModManager/0.1 (LoversLab Provider)");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION,
                     download::capture_content_disposition);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &p.content_disposition);

    const CURLcode res = curl_easy_perform(curl);
    (void)res;  // CURLE_WRITE_ERROR is expected (body discard)
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &p.http_code);
    char* eff = nullptr;
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &eff);
    if (eff) p.effective_url = eff;
    curl_easy_cleanup(curl);
    return p;
}

// "https://host/a/b/file.7z?x=1" -> "file.7z"
std::string url_path_basename(const std::string& url) {
    const std::size_t scheme = url.find("://");
    const std::size_t start = (scheme == std::string::npos) ? 0 : scheme + 3;
    const std::size_t slash = url.find('/', start);
    if (slash == std::string::npos) return {};
    const std::size_t q = url.find_first_of("?#", slash);
    std::string path = url.substr(
        slash + 1, (q == std::string::npos) ? std::string::npos : q - slash - 1);
    const std::size_t last = path.find_last_of('/');
    if (last != std::string::npos) path = path.substr(last + 1);
    return download::percent_decode(path);
}

std::string to_lower(const std::string& in) {
    std::string out = in;
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

} // namespace

bool LoversLabProvider::is_loverslab_url(const std::string& url) {
    std::size_t start = url.find("://");
    start = (start == std::string::npos) ? 0 : start + 3;
    const std::size_t end = url.find_first_of("/?#", start);
    std::string host = url.substr(
        start, (end == std::string::npos) ? std::string::npos : end - start);
    const std::size_t colon = host.find(':');
    if (colon != std::string::npos) host = host.substr(0, colon);
    host = to_lower(host);

    if (host != "loverslab.com" &&
        !(host.size() > 14 && host.compare(host.size() - 14, 14, ".loverslab.com") == 0))
        return false;
    return url.find("/files/file/") != std::string::npos;
}

std::string LoversLabProvider::extract_file_id(const std::string& url) {
    const std::string marker = "/files/file/";
    const std::size_t pos = url.find(marker);
    if (pos == std::string::npos) return {};
    const std::size_t start = pos + marker.size();
    const std::size_t seg_end = url.find_first_of("/?&", start);
    std::string seg = url.substr(
        start, (seg_end == std::string::npos) ? std::string::npos : seg_end - start);
    const std::size_t dash = seg.find('-');
    std::string id = (dash == std::string::npos) ? seg : seg.substr(0, dash);
    if (id.empty()) return {};
    for (const char c : id) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return {};
    }
    return id;
}

std::string LoversLabProvider::mod_page_url(const std::string& url) {
    const auto cut = url.find_first_of("?#");
    return url.substr(0, cut);
}

bool LoversLabProvider::fetch(const Mod& mod, PipelineContext& ctx,
                              const std::filesystem::path& dest_path) {
    if (mod.download_source_type != "loverslab") return false;

    if (mod.download_url.empty()) {
        Logger::instance().error("LoversLabProvider: no download URL");
        return false;
    }

    const std::string cookie = LoversLabAuth::instance().get_cookie();
    if (cookie.empty()) {
        Logger::instance().error(
            "LoversLabProvider: no session cookie configured "
            "(Settings > Sources > LoversLab)");
        return false;
    }

    download::Progress dp;
    dp.callback = ctx.on_progress;
    dp.should_abort = ctx.should_abort;
    dp.resume_base = ctx.download_resume_from;
    dp.start = std::chrono::steady_clock::now();

    download::Options opts;
    opts.cookie_header = cookie;
    opts.user_agent = "GameModManager/0.1 (LoversLab Provider)";
    opts.long_lived = true;

    std::string effective_url;
    opts.effective_url = &effective_url;

    long http_code = 0;
    bool aborted = false;
    if (!download::curl_download(mod.download_url, dest_path, http_code, opts,
                                 &dp, ctx.download_resume_from, &aborted)) {
        if (aborted) {
            // Pause requested - partial file is kept for resume.
            ctx.download_paused = true;
            Logger::instance().debug(
                "LoversLabProvider: download aborted (pause), partial kept at " +
                dest_path.string());
        } else if (http_code == 401 || http_code == 403) {
            Logger::instance().error(
                "LoversLabProvider: download rejected (HTTP " +
                std::to_string(http_code) +
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

    Logger::instance().debug(
        "LoversLabProvider: download complete -> " + dest_path.string() +
        " (final URL: " + (effective_url.empty() ? "<unknown>" : effective_url) + ")");
    return true;
}

SourceDownloadInfo LoversLabProvider::resolve_download_info(const Mod& mod) const {
    SourceDownloadInfo info;
    if (mod.download_source_type != "loverslab" || mod.download_url.empty())
        return info;

    const std::string cookie = LoversLabAuth::instance().get_cookie();
    if (cookie.empty()) return info;

    const Probe p = probe_download(mod.download_url, cookie);
    if (p.http_code == 401 || p.http_code == 403) {
        Logger::instance().warn(
            "LoversLabProvider: download-link probe rejected (HTTP " +
            std::to_string(p.http_code) +
            ") - the session cookie is expired or does not match the link");
        return info;
    }
    if (p.http_code >= 400) {
        Logger::instance().debug(
            "LoversLabProvider: download-link probe failed (HTTP " +
            std::to_string(p.http_code) + "), using default names");
        return info;
    }

    std::string fname =
        download::parse_content_disposition_filename(p.content_disposition);
    if (fname.empty()) fname = url_path_basename(p.effective_url);
    if (fname.empty()) return info;

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

std::string LoversLabProvider::display_name() const {
    return "LoversLab";
}

} // namespace engine
