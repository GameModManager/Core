#pragma once

#include <string>

namespace engine::Source::Http {

// libcurl rejects URLs carrying raw spaces/unsafe bytes. Nexus CDN download
// URLs embed the archive filename unencoded (e.g. "RaceMenu Anniversary
// Edition v0-4-20-0-...7z"), so the path segment must be percent-encoded
// before CURLOPT_URL. Scheme, host and query are left untouched; existing
// %XX escapes are preserved (no double-encoding).
std::string encode_url_path(const std::string& url);

// Decode the HTML entities the Nexus / LoversLab / mod.pub pages emit in
// human-readable metadata (category, author, titles): &amp; &lt; &gt;
// &quot; &apos; &nbsp; plus the numeric forms &#NNN; and &#xHH;.
//
// Single-pass on purpose (mirrors ui/modinfo/bbcode.cpp): re-scanning the
// output would collapse "&amp;amp;" into "&" and lose a literal "&amp;"
// the author intended. Unknown / malformed entities pass through untouched
// so the user sees the literal text instead of silently dropped chars.
//
// For full descriptions prefer the UI bbcode pipeline (bbcode_to_html),
// which already unescapes before parsing - decoding here as well would
// double-decode and break its single-pass guarantee. Use this only for
// short plain-text fields shown verbatim (category / name / author).
std::string decode_html_entities(const std::string& in);

} // namespace engine::Source::Http
