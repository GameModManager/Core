// =============================================================================
// engine::Nexus::Http::nexus_http_request - thin wrapper around Network::
// -----------------------------------------------------------------------------
// Preserved for the existing call sites (NexusProvider, NexusAccount, Nexus
// rate-limit parsing). Internally it delegates to the centralized Network::
// gateway so every Nexus API request shows up in the Debug panel Network tab.
//
// Behaviour parity with the pre-Network:: implementation:
//   * 30-second default timeout (overridable via timeout_seconds).
//   * application-name / application-version headers added on top of the
//     caller's curl_slist (Nexus AUP, help.nexusmods.com/article/114).
//   * Returns ok = (CURLcode == CURLE_OK); the caller still checks
//     http_code for the HTTP status.
//   * response_headers, when requested, capture the full headers block -
//     NexusAccount uses it to parse x-rl-* rate-limit headers.
// =============================================================================

#include "engine/source/nexus/http.h"

#include "engine/network/network_manager.h"

namespace engine::Source::Nexus::Http {

bool nexus_http_request(const std::string& url,
                        const std::string& post_body,
                        std::string& response_body,
                        long& http_code,
                        curl_slist* headers,
                        std::string* response_headers,
                        long timeout_seconds) {
    network::Request req;
    req.url = url;
    req.caller = NET_CALLER;
    req.timeout = std::chrono::seconds(timeout_seconds);
    req.body = post_body;
    if (!post_body.empty()) req.method = network::Method::Post;

    // Walk the legacy curl_slist and turn it into Network:: Headers. Each
    // entry is a "Name: value" line; the network manager will redact at log
    // time so secrets never reach the ring buffer.
    if (headers) {
        for (curl_slist* h = headers; h != nullptr; h = h->next) {
            if (h->data) req.headers.emplace_back(h->data);
        }
    }

    auto resp = network::instance().request(req);
    response_body = std::move(resp.body);
    http_code = resp.http_code;
    if (response_headers) *response_headers = std::move(resp.response_headers);
    // Surface the libcurl error string for diagnostics; Network:: sets it on
    // transport failures, so the caller can still log it.
    return resp.error.empty() && resp.http_code > 0;
}

} // namespace engine::Source::Nexus::Http