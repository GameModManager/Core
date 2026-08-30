#pragma once

#include <curl/curl.h>

#include <string>

namespace engine::Source::Nexus::Http {

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

} // namespace engine::Source::Nexus::Http
