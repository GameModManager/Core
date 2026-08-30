#pragma once

#include <string>

namespace engine::Source::Http {

// libcurl rejects URLs carrying raw spaces/unsafe bytes. Nexus CDN download
// URLs embed the archive filename unencoded (e.g. "RaceMenu Anniversary
// Edition v0-4-20-0-...7z"), so the path segment must be percent-encoded
// before CURLOPT_URL. Scheme, host and query are left untouched; existing
// %XX escapes are preserved (no double-encoding).
std::string encode_url_path(const std::string& url);

} // namespace engine::Source::Http
