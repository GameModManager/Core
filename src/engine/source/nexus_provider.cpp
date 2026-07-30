#include "engine/source/nexus_provider.h"
#include "engine/model/mod.h"
#include "engine/pipeline/pipeline.h"
#include "engine/nxm/nxm_router.h"
#include "engine/log/logger.h"
#include "engine/nexus_auth.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cctype>
#include <fstream>
#include <sstream>

namespace engine {

// ---------------------------------------------------------------------------
// Progress callback for libcurl downloads
// ---------------------------------------------------------------------------

struct DownloadProgress {
    std::function<void(int64_t, int64_t, double)> callback;
    std::chrono::steady_clock::time_point start;
};

static int xferinfo_callback(void* user_data, curl_off_t dltotal, curl_off_t dlnow,
                              curl_off_t ultotal, curl_off_t ulnow) {
    (void)ultotal; (void)ulnow;
    auto* dp = static_cast<DownloadProgress*>(user_data);
    if (dp && dp->callback) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration<double>(now - dp->start).count();
        double speed = (elapsed > 0.001) ? (dlnow / elapsed) : 0.0;
        dp->callback(dlnow, dltotal, speed);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// libcurl write callbacks
// ---------------------------------------------------------------------------

static size_t write_to_file(void* ptr, size_t size, size_t nmemb, void* stream) {
    auto* file = static_cast<std::ofstream*>(stream);
    if (!file || !file->is_open()) return 0;
    auto written = static_cast<std::streamsize>(size * nmemb);
    file->write(static_cast<const char*>(ptr), written);
    return file->good() ? (size * nmemb) : 0;
}

static size_t write_to_string(void* ptr, size_t size, size_t nmemb, void* stream) {
    auto* str = static_cast<std::string*>(stream);
    str->append(static_cast<const char*>(ptr), size * nmemb);
    return size * nmemb;
}

static size_t header_callback(void* ptr, size_t size, size_t nmemb, void* user_data) {
    auto* headers = static_cast<std::string*>(user_data);
    headers->append(static_cast<const char*>(ptr), size * nmemb);
    return size * nmemb;
}

// ---------------------------------------------------------------------------
// Helper: perform a libcurl request and return response body
// ---------------------------------------------------------------------------

static bool curl_request(const std::string& url,
                         const std::string& post_body,
                         std::string& response_body,
                         long& http_code,
                         curl_slist* headers = nullptr,
                         std::string* response_headers = nullptr) {
    auto* curl = curl_easy_init();
    if (!curl) return false;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                     "GameModManager/0.1 (Nexus Provider)");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);

    if (headers)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    if (response_headers) {
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, response_headers);
    }

    if (!post_body.empty()) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, post_body.size());
    }

    CURLcode res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    return (res == CURLE_OK);
}

static bool curl_download(const std::string& url,
                          const std::filesystem::path& dest_path,
                          long& http_code,
                          DownloadProgress* progress = nullptr) {
    auto* curl = curl_easy_init();
    if (!curl) return false;

    std::ofstream file(dest_path, std::ios::binary);
    if (!file) {
        curl_easy_cleanup(curl);
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                     "GameModManager/0.1 (Nexus Provider)");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &file);

    if (progress && progress->callback) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferinfo_callback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, progress);
    }

    CURLcode res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    file.close();
    if (res != CURLE_OK || http_code >= 400) {
        std::error_code ec;
        std::filesystem::remove(dest_path, ec);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Helper: parse rate-limit headers from a raw header block
// ---------------------------------------------------------------------------

static void parse_rate_limits(const std::string& headers) {
    auto find_header = [&](const std::string& name) -> int64_t {
        auto pos = headers.find(name + ": ");
        if (pos == std::string::npos) {
            // try lowercase
            std::string lower = name;
            for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            pos = headers.find(lower + ": ");
            if (pos == std::string::npos) return -1;
        }
        pos = headers.find(':', pos) + 1;
        while (pos < headers.size() && headers[pos] == ' ') ++pos;
        int64_t val = 0;
        while (pos < headers.size() && headers[pos] >= '0' && headers[pos] <= '9') {
            val = val * 10 + (headers[pos] - '0');
            ++pos;
        }
        return val;
    };

    // Only parse authenticated headers (the ones that matter for API-key users)
    int64_t limit      = find_header("x-rl-authenticated-limit");
    int64_t remaining  = find_header("x-rl-authenticated-remaining");
    int64_t reset      = find_header("x-rl-authenticated-reset");

    if (limit > 0 && remaining >= 0 && reset > 0) {
        NexusAuth::instance().update_rate_limit(
            static_cast<int>(limit),
            static_cast<int>(remaining),
            reset);
    }
}

// ---------------------------------------------------------------------------
// NexusProvider::fetch
// ---------------------------------------------------------------------------

bool NexusProvider::fetch(const Mod& mod, const PipelineContext& ctx,
                          const std::filesystem::path& dest_path) {
    if (mod.download_source_type != "nexus") return false;

    const auto& nxm = mod.download_nxm;
    if (nxm.file_id <= 0) {
        Logger::instance().error("NexusProvider: invalid file_id");
        return false;
    }

    bool use_api_key  = nxm.key.empty() && NexusAuth::instance().has_api_key();
    bool use_nxm_auth = !nxm.key.empty();

    if (!use_api_key && !use_nxm_auth) {
        Logger::instance().error(
            "NexusProvider: no NXM download key and no API key configured");
        return false;
    }

    std::string download_url;

    // ---- Step 1: Resolve a direct download URL ----

    if (use_api_key) {
        // -- API-key path ----------------------------------------------
        std::string api_url =
            "https://api.nexusmods.com/v1/games/"
            + nxm.nexus_domain + "/mods/"
            + mod.download_source_id + "/files/"
            + std::to_string(nxm.file_id) + "/download_link.json";

        std::string api_key = NexusAuth::instance().get_api_key();
        if (api_key.empty()) {
            Logger::instance().error("NexusProvider: API key file exists but is empty");
            return false;
        }

        curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, ("apikey: " + api_key).c_str());
        headers = curl_slist_append(headers, "Accept: application/json");

        std::string response;
        std::string resp_headers;
        long http_code = 0;

        bool ok = curl_request(api_url, "", response, http_code, headers, &resp_headers);
        curl_slist_free_all(headers);

        if (resp_headers.size() > 20)  // sanity check — don't parse empty/trivial
            parse_rate_limits(resp_headers);

        if (!ok) {
            Logger::instance().error("NexusProvider: API-key request failed (curl error)");
            return false;
        }
        if (http_code == 403) {
            Logger::instance().error(
                "NexusProvider: API key rejected (HTTP 403) — check your key at "
                "nexusmods.com/users/myaccount?tab=api");
            return false;
        }
        if (http_code != 200) {
            Logger::instance().error("NexusProvider: Nexus API returned HTTP " +
                                     std::to_string(http_code) + " for API-key request");
            return false;
        }

        try {
            auto j = nlohmann::json::parse(response);
            if (!j.is_array() || j.empty()) {
                Logger::instance().error("NexusProvider: unexpected API-key response format");
                return false;
            }
            download_url = j[0].value("URI", "");
            if (download_url.empty())
                download_url = j[0].value("download_url", "");
        } catch (const std::exception& e) {
            Logger::instance().error("NexusProvider: failed to parse API-key response: " +
                                     std::string(e.what()));
            return false;
        }

        if (download_url.empty()) {
            Logger::instance().error("NexusProvider: empty download URL in API-key response");
            return false;
        }

    } else {
        // -- NXM auth path (existing) ----------------------------------
        std::ostringstream body;
        body << "fid=" << nxm.file_id
             << "&game_id=" << nxm.nexus_domain
             << "&key=" << nxm.key
             << "&expire=" << nxm.expire
             << "&user_id=" << nxm.user_id;

        std::string api_response;
        long http_code = 0;

        bool ok = curl_request(
            "https://www.nexusmods.com/Core/Libs/Common/Managers/"
            "Downloads?GenerateDownloadUrl",
            body.str(), api_response, http_code);

        if (!ok) {
            Logger::instance().error("NexusProvider: HTTP request failed (curl error)");
            return false;
        }
        if (http_code != 200) {
            Logger::instance().error("NexusProvider: Nexus API returned HTTP " +
                                     std::to_string(http_code));
            return false;
        }

        try {
            auto j = nlohmann::json::parse(api_response);
            if (j.contains("url") && !j["url"].is_null()) {
                download_url = j["url"].get<std::string>();
            } else {
                std::string status = j.value("status", "unknown");
                Logger::instance().error("NexusProvider: API returned status=" + status);
                return false;
            }
        } catch (const std::exception& e) {
            Logger::instance().error("NexusProvider: failed to parse API response: " +
                                     std::string(e.what()));
            return false;
        }

        if (download_url.empty()) {
            Logger::instance().error("NexusProvider: empty download URL in API response");
            return false;
        }
    }

    // ---- Step 2: Download the file ----
    Logger::instance().debug("NexusProvider: downloading from Nexus...");
    long dl_code = 0;

    DownloadProgress dp;
    dp.callback = ctx.on_progress;
    dp.start = std::chrono::steady_clock::now();

    if (!curl_download(download_url, dest_path, dl_code, &dp)) {
        Logger::instance().error("NexusProvider: download failed (HTTP " +
                                 std::to_string(dl_code) + ")");
        return false;
    }

    Logger::instance().debug("NexusProvider: download complete -> " + dest_path.string());
    return true;
}

} // namespace engine
