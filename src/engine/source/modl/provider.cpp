#include "engine/source/modl/provider.h"

#include "engine/core/log/logger.h"
#include "engine/mod/model/mod.h"
#include "engine/pipeline/pipeline.h"
#include "engine/source/download/curl_download.h"
#include "engine/source/download/manager.h"

#include <chrono>
#include <filesystem>
#include <string>

namespace engine::Source::Modl {

namespace {

// Same basename-extraction loverslab uses for its effective_url: the last
// path segment with query/fragment stripped, percent-decoded.
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
    return DownloadManager::percent_decode(path);
}

} // namespace

bool Provider::fetch(const Mod& mod, PipelineContext& ctx,
                     const std::filesystem::path& dest_path) {
    if (mod.download_source_type != "modl") return false;
    if (mod.download_url.empty()) {
        Logger::instance().error("ModlProvider: no download URL");
        return false;
    }

    // modl:// links are assumed to point at a plain https archive (mod.pub
    // does this in the article). v1 sends no cookie; sites that need auth
    // can be added later by mirroring LoversLab's cookie_header pattern.
    DownloadManager::Progress dp;
    dp.callback = ctx.on_progress;
    dp.should_abort = ctx.should_abort;
    dp.resume_base = ctx.download_resume_from;
    dp.start = std::chrono::steady_clock::now();

    DownloadManager::Options opts;
    opts.user_agent = "GameModManager/0.1 (modl Provider)";
    opts.long_lived = true;

    long http_code = 0;
    bool aborted = false;
    if (!DownloadManager::curl_download(mod.download_url, dest_path, http_code,
                                        opts, &dp, ctx.download_resume_from,
                                        &aborted)) {
        if (aborted) {
            ctx.download_paused = true;
            Logger::instance().debug(
                "ModlProvider: download aborted (pause), partial kept at " +
                dest_path.string());
        } else if (http_code == 401 || http_code == 403) {
            Logger::instance().error(
                "ModlProvider: download rejected (HTTP " +
                std::to_string(http_code) +
                ") - the host may require authentication (not yet supported "
                "for modl:// links)");
        } else {
            Logger::instance().error(
                "ModlProvider: download failed (HTTP " +
                std::to_string(http_code) + ")");
        }
        return false;
    }
    Logger::instance().debug("ModlProvider: download complete -> " +
                             dest_path.string());
    return true;
}

SourceDownloadInfo Provider::resolve_download_info(const Mod& mod) const {
    SourceDownloadInfo info;
    if (mod.download_source_type != "modl" || mod.download_url.empty())
        return info;

    // The modl link is the direct download URL with no extra API call, so the
    // basename is the best fallback we can do without an HTTP probe (which
    // would be a wasted network round-trip on every queue). Real
    // Content-Disposition probes are reserved for sources that lack a known
    // file name (Nexus API). The basename still gives the Downloads tab a
    // reasonable placeholder when nothing else resolves a name.
    const std::string fname = url_path_basename(mod.download_url);
    if (fname.empty()) return info;
    info.archive_name = fname;
    info.display_name = std::filesystem::path(fname).stem().string();
    return info;
}

std::string Provider::display_name() const {
    return "Modl";
}

} // namespace engine::Source::Modl
