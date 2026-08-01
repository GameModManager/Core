#pragma once

#include <curl/curl.h>

#include <string>

namespace engine {

// Shared libcurl helper for all Nexus API calls (download-link resolution,
// file metadata, account validation). Appends the Nexus AUP identification
// headers (application-name/application-version, help.nexusmods.com/article/114)
// on top of any caller-supplied headers so the apikey and Accept headers
// survive. Returns true when the transfer itself succeeded; check http_code
// separately for the HTTP status.
bool nexus_http_request(const std::string& url,
                        const std::string& post_body,
                        std::string& response_body,
                        long& http_code,
                        curl_slist* headers = nullptr,
                        std::string* response_headers = nullptr,
                        long timeout_seconds = 30);

// libcurl rejects URLs carrying raw spaces/unsafe bytes. Nexus CDN download
// URLs embed the archive filename unencoded (e.g. "RaceMenu Anniversary
// Edition v0-4-20-0-...7z"), so the path segment must be percent-encoded
// before CURLOPT_URL. Scheme, host and query are left untouched; existing
// %XX escapes are preserved (no double-encoding).
std::string encode_url_path(const std::string& url);

} // namespace engine
