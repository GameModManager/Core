#include "engine/source/nexus_provider.h"
#include "engine/source/download/curl_download.h"
#include "engine/mod/model/mod.h"
#include "engine/pipeline/pipeline.h"
#include "engine/source/nxm/nxm_router.h"
#include "engine/core/log/logger.h"
#include "engine/source/nexus_auth.h"
#include "engine/source/nexus_account.h"
#include "engine/source/nexus_http.h"
#include "engine/source/nexus_servers.h"
#include "engine/network/network_manager.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cctype>
#include <sstream>

namespace engine::Source::Nexus {

static bool contains_ci(const std::string& haystack, const std::string& needle) {
    std::string h = haystack, n = needle;
    for (auto& c : h) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (auto& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return h.find(n) != std::string::npos;
}

// ---------------------------------------------------------------------------
// Provider::fetch
// ---------------------------------------------------------------------------

bool Provider::fetch(const Mod& mod, PipelineContext& ctx,
                     const std::filesystem::path& dest_path) {
    Logger::instance().debug("[NexusProvider] fetch called: mod_id=" + mod.download_source_id +
                             " file_id=" + std::to_string(mod.download_nxm.file_id) +
                             " domain=" + mod.download_nxm.nexus_domain);
    if (mod.download_source_type != "nexus") {
        Logger::instance().warn("[NexusProvider] fetch called with non-nexus source_type: " + mod.download_source_type);
        return false;
    }

    const auto& nxm = mod.download_nxm;

    // Direct-URL path: the caller already resolved a working download URL, so
    // no API auth or URL resolution is needed.
    if (!mod.download_url.empty()) {
        Logger::instance().debug("[NexusProvider] Direct URL path, calling download_from_url");
        return download_from_url(mod.download_url, ctx, dest_path);
    }

    if (nxm.file_id <= 0) {
        Logger::instance().error("[NexusProvider] Invalid file_id: " + std::to_string(nxm.file_id));
        return false;
    }

    bool use_api_key  = nxm.key.empty() && Auth::instance().has_api_key();
    bool use_nxm_auth = !nxm.key.empty();

    Logger::instance().debug("[NexusProvider] Auth: use_api_key=" + std::string(use_api_key ? "true" : "false") +
                             " use_nxm_auth=" + std::string(use_nxm_auth ? "true" : "false") +
                             " has_api_key=" + std::string(Auth::instance().has_api_key() ? "true" : "false") +
                             " key_empty=" + std::string(nxm.key.empty() ? "true" : "false"));

    if (!use_api_key && !use_nxm_auth) {
        Logger::instance().error(
            "[NexusProvider] No NXM download key and no API key configured");
        return false;
    }

    std::string download_url;
    std::string server_name;

    // Collects the download_link entries, records each as a known server, then
    // picks the most preferred URL. Mirrors MO2's ServerByPreference ordering:
    // the user's preferred mirrors first (rank), then the CDN, then the rest
    // in the API's own order. Fills download_url + server_name.
    auto parse_and_pick = [&](const std::string& body) -> bool {
        try {
            auto j = nlohmann::json::parse(body);
            if (!j.is_array()) {
                Logger::instance().error(
                    "[NexusProvider] Unexpected download-link response format (not array)");
                return false;
            }

            struct Entry {
                std::string name;
                bool premium = false;
                std::string uri;
            };
            std::vector<Entry> entries;
            for (const auto& e : j) {
                if (!e.is_object()) continue;
                std::string uri = e.value("URI", "");
                if (uri.empty()) uri = e.value("download_url", "");
                if (uri.empty()) continue;

                std::string long_name = e.value("name", "");
                std::string name = e.value("short_name", "");
                if (name.empty()) name = long_name;
                const bool premium = contains_ci(long_name, "Premium");
                if (!name.empty())
                    Servers::instance().record_discovered(name, premium);
                entries.push_back({name, premium, uri});
            }

            if (entries.empty()) {
                Logger::instance().error(
                    "[NexusProvider] No usable download server in response");
                return false;
            }

            std::stable_sort(entries.begin(), entries.end(),
                             [](const Entry& a, const Entry& b) {
                const int pa = Servers::instance().preferred_rank(a.name);
                const int pb = Servers::instance().preferred_rank(b.name);
                if (pa != pb) return pa > pb;
                const bool cda = contains_ci(a.name, "CDN");
                const bool cdb = contains_ci(b.name, "CDN");
                if (cda != cdb) return cda;
                return false;  // preserve the API's order for the rest
            });

            download_url = entries.front().uri;
            server_name = entries.front().name;
            Logger::instance().debug("[NexusProvider] Selected server: " + server_name + " url=" + download_url);
            return true;
        } catch (const std::exception& e) {
            Logger::instance().error(
                "[NexusProvider] Failed to parse download-link response: " +
                std::string(e.what()));
            return false;
        }
    };

    // ---- Step 1: Resolve a direct download URL ----

    if (use_api_key) {
        // -- API-key path ----------------------------------------------
        std::string api_url =
            "https://api.nexusmods.com/v1/games/"
            + nxm.nexus_domain + "/mods/"
            + mod.download_source_id + "/files/"
            + std::to_string(nxm.file_id) + "/download_link.json";

        Logger::instance().debug("[NexusProvider] API-key path: requesting download_link.json for mod=" + mod.download_source_id + " file=" + std::to_string(nxm.file_id));

        std::string api_key = Auth::instance().get_api_key();
        if (api_key.empty()) {
            Logger::instance().error("[NexusProvider] API key file exists but is empty");
            return false;
        }

        curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, ("apikey: " + api_key).c_str());
        headers = curl_slist_append(headers, "Accept: application/json");

        std::string response;
        std::string resp_headers;
        long http_code = 0;

        bool ok = Http::nexus_http_request(api_url, "", response, http_code, headers, &resp_headers);
        curl_slist_free_all(headers);

        if (resp_headers.size() > 20)  // sanity check - don't parse empty/trivial
            Account::parse_rate_limits(resp_headers);

        if (!ok) {
            Logger::instance().error("[NexusProvider] API-key request failed (curl error)");
            return false;
        }
        if (http_code == 403) {
            Logger::instance().error(
                "[NexusProvider] API key rejected (HTTP 403) - check your key at "
                "nexusmods.com/users/myaccount?tab=api");
            return false;
        }
        if (http_code != 200) {
            Logger::instance().error("[NexusProvider] Nexus API returned HTTP " +
                                     std::to_string(http_code) + " for API-key request");
            return false;
        }

        if (!parse_and_pick(response)) return false;

    } else {
        // -- NXM auth path (signed-link flow, MO2-compatible) ----------
        // The nxm:// link carries a signed key+expires token minted when the
        // user visited nexusmods.com. That token - not the API key - is what
        // authorizes a download for non-premium accounts, so it is passed as
        // query params to the API's download_link endpoint (note: NOT
        // download_link.json, which is premium-only). The API key still
        // authenticates the caller on top of it. The old web endpoint
        // ("Managers/Downloads?GenerateDownloadUrl") was Cloudflare-gated and
        // returned an anti-bot page instead of a URL.
        std::string api_url =
            "https://api.nexusmods.com/v1/games/"
            + nxm.nexus_domain + "/mods/"
            + mod.download_source_id + "/files/"
            + std::to_string(nxm.file_id) + "/download_link";

        Logger::instance().debug("[NexusProvider] NXM-auth path: requesting download_link for mod=" + mod.download_source_id + " file=" + std::to_string(nxm.file_id));

        api_url += "?key=" + nxm.key;
        if (nxm.expire > 0)
            api_url += "&expires=" + std::to_string(nxm.expire);

        curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Accept: application/json");
        std::string api_key = Auth::instance().get_api_key();
        if (!api_key.empty())
            headers = curl_slist_append(headers, ("apikey: " + api_key).c_str());

        std::string api_response;
        std::string resp_headers;
        long http_code = 0;

        bool ok = Http::nexus_http_request(api_url, "", api_response, http_code, headers, &resp_headers);
        curl_slist_free_all(headers);

        if (resp_headers.size() > 20)  // sanity check - don't parse empty/trivial
            Account::parse_rate_limits(resp_headers);

        if (!ok) {
            Logger::instance().error("[NexusProvider] NXM-auth request failed (curl error)");
            return false;
        }
        if (http_code == 403) {
            Logger::instance().error(
                "NexusProvider: download_link rejected (HTTP 403) - the NXM key may be "
                "expired, or the link was generated by a different account than the API "
                "key. Re-request the download link from nexusmods.com.");
            return false;
        }
        if (http_code != 200) {
            Logger::instance().error("NexusProvider: Nexus API returned HTTP " +
                                     std::to_string(http_code) +
                                     " for NXM-auth request");
            return false;
        }

        if (!parse_and_pick(api_response)) return false;
    }

    // ---- Step 2: Download the file ----
    Logger::instance().debug("[NexusProvider] Step 2: Downloading file from URL: " + download_url);
    return download_from_url(download_url, ctx, dest_path, server_name);
}

bool Provider::download_from_url(const std::string& download_url,
                                 PipelineContext& ctx,
                                 const std::filesystem::path& dest_path,
                                 const std::string& server_name) {
    Logger::instance().debug("[NexusProvider] download_from_url: url=" + download_url +
                             " dest=" + dest_path.string() +
                             " server=" + server_name +
                             " resume_from=" + std::to_string(ctx.download_resume_from));
    long dl_code = 0;

    engine::download::Progress dp;
    dp.callback = ctx.on_progress;
    dp.should_abort = ctx.should_abort;
    dp.resume_base = ctx.download_resume_from;
    dp.start = std::chrono::steady_clock::now();

    // Large archives routinely exceed a fixed transfer timeout (Nexus is
    // often slow). long_lived removes the overall cap - only the connect
    // timeout applies - matching LoversLabProvider's large-file handling.
    engine::download::Options opts;
    opts.long_lived = true;

    bool aborted = false;
    Logger::instance().debug("[NexusProvider] Calling engine::download::curl_download");
    if (!engine::download::curl_download(
            download_url, dest_path, dl_code, opts, &dp,
            ctx.download_resume_from, &aborted, NET_CALLER)) {
        if (aborted) {
            // Pause requested - partial file is kept for resume.
            ctx.download_paused = true;
            Logger::instance().debug(
                "[NexusProvider] Download aborted (pause), partial kept at " +
                dest_path.string());
        } else {
            Logger::instance().error("[NexusProvider] Download failed (HTTP " +
                                     std::to_string(dl_code) + ")");
        }
        return false;
    }

    // MO2 parity: record a speed sample for the serving server so the Servers
    // settings box can show a per-mirror average. Short downloads (<5s) are
    // skipped, their rate is too imprecise.
    if (!server_name.empty()) {
        const auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - dp.start).count();
        std::error_code ec;
        const auto size = std::filesystem::file_size(dest_path, ec);
        if (!ec && elapsed > 5.0) {
            const double bytes = static_cast<double>(size)
                                 - static_cast<double>(dp.resume_base);
            if (bytes > 0)
                Servers::instance().record_speed(server_name, bytes / elapsed);
        }
    }

    Logger::instance().debug("[NexusProvider] Download complete -> " + dest_path.string());
    return true;
}

// ---------------------------------------------------------------------------
// Provider::resolve_download_info
// ---------------------------------------------------------------------------

SourceDownloadInfo Provider::resolve_download_info(const Mod& mod) const {
    SourceDownloadInfo info;
    if (mod.download_source_type != "nexus") return info;
    const auto& nxm = mod.download_nxm;
    if (nxm.file_id <= 0 || mod.download_source_id.empty()) return info;

    // The file-metadata endpoint works on free accounts (unlike
    // download_link.json); it needs the API key. No key -> keep the default
    // names (fetch() will fail anyway without one, except via NXM auth).
    if (!Auth::instance().has_api_key()) return info;

    std::string api_key = Auth::instance().get_api_key();
    if (api_key.empty()) return info;

    std::string url =
        "https://api.nexusmods.com/v1/games/"
        + nxm.nexus_domain + "/mods/"
        + mod.download_source_id + "/files/"
        + std::to_string(nxm.file_id) + ".json";

    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("apikey: " + api_key).c_str());
    headers = curl_slist_append(headers, "Accept: application/json");

    std::string response;
    std::string resp_headers;
    long http_code = 0;
    bool ok = Http::nexus_http_request(url, "", response, http_code, headers, &resp_headers);
    curl_slist_free_all(headers);

    if (resp_headers.size() > 20)  // sanity check - don't parse empty/trivial
        Account::parse_rate_limits(resp_headers);

    if (!ok || http_code != 200) {
        Logger::instance().debug(
            "NexusProvider: file metadata request failed (HTTP " +
            std::to_string(http_code) + "), using default names");
        return info;
    }

    try {
        auto j = nlohmann::json::parse(response);
        std::string archive_name = j.value("file_name", "");
        if (archive_name.find('/') != std::string::npos ||
            archive_name.find('\\') != std::string::npos) {
            Logger::instance().debug(
                "NexusProvider: file metadata returned no usable file_name");
            return info;
        }
        info.archive_name = archive_name;
        info.display_name = j.value("name", "");
        return info;
    } catch (const std::exception& e) {
        Logger::instance().debug(
            "NexusProvider: failed to parse file metadata: " +
            std::string(e.what()));
        return info;
    }
}

std::string Provider::display_name() const {
    return "Nexus Mods";
}

// ---------------------------------------------------------------------------
// Provider::fetch_mod_info
// ---------------------------------------------------------------------------

ModInfoResult Provider::fetch_mod_info(const std::string& nexus_domain,
                                       const std::string& mod_id) const {
    ModInfoResult result;
    if (nexus_domain.empty() || mod_id.empty()) return result;
    if (!Auth::instance().has_api_key()) {
        Logger::instance().debug(
            "NexusProvider: fetch_mod_info skipped (no API key configured)");
        return result;
    }

    const std::string api_key = Auth::instance().get_api_key();
    if (api_key.empty()) return result;

    const std::string url = "https://api.nexusmods.com/v1/games/"
                            + nexus_domain + "/mods/" + mod_id + ".json";

    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("apikey: " + api_key).c_str());
    headers = curl_slist_append(headers, "Accept: application/json");

    std::string response;
    std::string resp_headers;
    long http_code = 0;
    const bool ok = Http::nexus_http_request(url, "", response, http_code, headers,
                                       &resp_headers);
    curl_slist_free_all(headers);

    if (resp_headers.size() > 20)  // sanity check - don't parse empty/trivial
        Account::parse_rate_limits(resp_headers);

    if (!ok || http_code != 200) {
        Logger::instance().debug(
            "NexusProvider: fetch_mod_info failed (HTTP " +
            std::to_string(http_code) + ") for mod " + mod_id);
        return result;
    }

    return parse_mod_info(response);
}

ModInfoResult Provider::parse_mod_info(const std::string& body) {
    ModInfoResult result;
    try {
        auto j = nlohmann::json::parse(body);
        if (!j.is_object()) return result;
        result.available = j.value("available", false);
        result.name = j.value("name", "");
        result.version = j.value("version", "");
        result.newest_version = j.value("newest_version", "");
        if (j.contains("category_id")) {
            const auto& c = j["category_id"];
            if (c.is_number())
                result.category_id = std::to_string(c.get<int>());
            else if (c.is_string())
                result.category_id = c.get<std::string>();
        }
        result.description = j.value("description", "");
        result.author = j.value("author", "");
        return result;
    } catch (const std::exception&) {
        return result;
    }
}

} // namespace engine::Source::Nexus
