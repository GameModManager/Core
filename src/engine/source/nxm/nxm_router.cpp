#include "engine/source/nxm/nxm_router.h"

#include "engine/source/download/manager.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::Source {

NxmLink Router::parse(const std::string& url) {
    NxmLink link;
    link.full_url = url;

    // Expected: nxm://<domain>/mods/<mod_id>/files/<file_id>?key=...&expire=...&user_id=...
    // Or:       nxm://<domain>/mods/<mod_id>/files/<file_id>&key=...
    // The domain is everything after "nxm://" and before the first '/'

    const std::string prefix = "nxm://";
    if (url.size() <= prefix.size()) return link;
    if (url.substr(0, prefix.size()) != prefix) return link;

    auto rest = url.substr(prefix.size());

    // Tolerate an empty authority re-serialized as a leading slash by browser
    // / portal layers: "nxm:///game/mods/..." is the same link as
    // "nxm://game/mods/...". Strip any leading slashes.
    while (rest.size() > 1 && rest[0] == '/') rest.erase(0, 1);

    // Extract domain (up to first '/' or '?')
    auto domain_end = rest.find_first_of("/?");
    if (domain_end == std::string::npos) {
        // Just a domain, no path - e.g. "nxm://isaac"
        link.nexus_domain = rest;
        return link;
    }
    link.nexus_domain = rest.substr(0, domain_end);
    if (link.nexus_domain.empty()) return link;

    auto path = rest.substr(domain_end + 1);

    // The Nexus site emits "nxm://nexus/<game-domain>/mods/...": the literal
    // authority "nexus" is not a game domain, the real one follows it.
    if (rest.substr(0, domain_end) == "nexus") {
        auto second_end = path.find_first_of("/?");
        if (second_end != std::string::npos) {
            auto second = path.substr(0, second_end);
            if (second != "mods" && second != "files") {
                link.nexus_domain = second;
                path = path.substr(second_end + 1);
            }
        }
    }

    // Parse path segments: mods/<mod_id>/files/<file_id>
    // Split on '/' and walk pairs
    std::vector<std::string> segments;
    size_t pos = 0;
    while (pos < path.size()) {
        auto slash = path.find('/', pos);
        if (slash == std::string::npos) {
            segments.push_back(path.substr(pos));
            break;
        }
        segments.push_back(path.substr(pos, slash - pos));
        pos = slash + 1;
    }

    // Look for "mods" keyword and extract mod_id after it
    for (size_t i = 0; i + 1 < segments.size(); ++i) {
        if (segments[i] == "mods") {
            try { link.mod_id = std::stoll(segments[i + 1]); }
            catch (...) {}
            break;
        }
    }

    // Look for "files" keyword and extract file_id after it
    for (size_t i = 0; i + 1 < segments.size(); ++i) {
        if (segments[i] == "files") {
            // file_id may have query params appended - strip at '?' or '&'
            auto& raw = segments[i + 1];
            auto qpos = raw.find_first_of("?&");
            auto id_str = (qpos != std::string::npos) ? raw.substr(0, qpos) : raw;
            try { link.file_id = std::stoll(id_str); }
            catch (...) {}
            break;
        }
    }

    // Parse query parameters: key=..., expire=..., user_id=...
    auto qpos = path.find('?');
    if (qpos == std::string::npos) {
        // Also check for '&' in the file_id segment (nxm uses both ? and &)
        for (const auto& seg : segments) {
            auto amp = seg.find('&');
            if (amp != std::string::npos) {
                // Parse from here
                auto query = seg.substr(amp + 1);
                // Also check previous segments for '?'
                // Actually, let's just find the query string in the full path
                break;
            }
        }
    }

    // Find query string - could start with '?' or be embedded with '&'
    {
        std::string query;
        auto q1 = url.find('?');
        if (q1 != std::string::npos) {
            query = url.substr(q1 + 1);
        } else {
            // nxm links sometimes use & without ? - find "key=" anywhere
            auto key_pos = url.find("key=");
            if (key_pos != std::string::npos) {
                // Walk backwards to find the delimiter before "key="
                while (key_pos > 0 && url[key_pos - 1] != '?' && url[key_pos - 1] != '&') {
                    --key_pos;
                }
                if (key_pos > 0) query = url.substr(key_pos);
            }
        }

        // Parse key=value pairs separated by '&' or '&amp;'
        auto parse_kv = [&](const std::string& qs) {
            size_t p = 0;
            while (p < qs.size()) {
                auto eq = qs.find('=', p);
                if (eq == std::string::npos) break;
                auto amp = qs.find_first_of("&", eq + 1);
                auto val = (amp != std::string::npos)
                    ? qs.substr(eq + 1, amp - eq - 1)
                    : qs.substr(eq + 1);

                auto key = qs.substr(p, eq - p);
                // Strip HTML entities
                auto strip = [](std::string s) {
                    auto pos = s.find("&amp;");
                    while (pos != std::string::npos) {
                        s.replace(pos, 5, "&");
                        pos = s.find("&amp;", pos + 1);
                    }
                    return s;
                };
                key = strip(key);
                val = strip(val);

                if (key == "key") link.key = val;
                else if (key == "expire" || key == "expires") {
                    try { link.expire = std::stoll(val); } catch (...) {}
                }
                else if (key == "user_id") {
                    try { link.user_id = std::stoll(val); } catch (...) {}
                }

                if (amp != std::string::npos) p = amp + 1;
                else break;
            }
        };
        if (!query.empty()) parse_kv(query);
    }

    return link;
}

std::string Router::match_game(
    const std::string& nexus_domain,
    const std::vector<std::pair<std::string, std::string>>& plugin_domains) {
    // plugin_domains: vector of (game_id, nexus_domain) from loaded plugins
    for (const auto& [game_id, domain] : plugin_domains) {
        if (domain == nexus_domain) return game_id;
    }
    return {};
}

namespace {

// MO2 GameShortName alias map. Mirrors modlhandler's HandlerStorage::knownGames
// (case-insensitive on input, normalized to the canonical game_id the GMM
// plugin registers). Keep the keys lowercase - normalize_modl_game_id lower-
// cases input first.
const std::unordered_map<std::string, std::string>& modl_aliases() {
    static const std::unordered_map<std::string, std::string> kMap = {
        {"morrowind", "morrowind"},
        {"oblivion", "oblivion"},
        {"fallout3", "fallout3"},
        {"fallout4", "fallout4"},
        {"falloutnv", "falloutnv"},
        {"newvegas", "falloutnv"},
        {"skyrim", "skyrim"},
        {"skyrimse", "skyrimse"},
        {"skyrimspecialedition", "skyrimse"},
        {"enderal", "enderal"},
        {"enderalse", "enderalse"},
        {"enderalspecialedition", "enderalse"},
        {"starfield", "starfield"},
        {"other", "other"},
    };
    return kMap;
}

std::string to_lower_ascii(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

std::string Router::normalize_modl_game_id(const std::string& host) {
    const auto& m = modl_aliases();
    const std::string lower = to_lower_ascii(host);
    auto it = m.find(lower);
    if (it != m.end()) return it->second;
    return lower;  // passthrough (lowercased) - caller decides if it matches a plugin
}

// ponytail: only_ascii - case-insensitive ASCII compare via the two-arg form
// of std::equal. Rejects HTTPS://, Https://, etc. without a temporary copy.
static bool istarts_with_ascii(const std::string& s, const char* prefix) {
    const std::size_t n = std::strlen(prefix);
    if (s.size() < n) return false;
    for (std::size_t i = 0; i < n; ++i) {
        if (static_cast<unsigned char>(s[i]) !=
            static_cast<unsigned char>(prefix[i])) {
            // Allow case-insensitive only for the scheme letters [A-Za-z]
            const unsigned char a = static_cast<unsigned char>(s[i]);
            const unsigned char b = static_cast<unsigned char>(prefix[i]);
            if (std::tolower(a) != std::tolower(b)) return false;
        }
    }
    return true;
}

ModlLink Router::parse_modl(const std::string& url) {
    ModlLink link;
    link.full_url = url;

    // modl://<host>/?url=<percent-encoded https://...>
    // Tolerate modl:/// (browser/portal layers) the same way parse() does.
    static const std::string prefix = "modl://";
    if (url.size() <= prefix.size()) return link;
    if (url.compare(0, prefix.size(), prefix) != 0) return link;

    auto rest = url.substr(prefix.size());
    while (rest.size() > 1 && rest[0] == '/') rest.erase(0, 1);

    // Host = up to first '/', '?'.
    const auto host_end = rest.find_first_of("/?#");
    const std::string host =
        (host_end == std::string::npos) ? rest : rest.substr(0, host_end);
    if (host.empty()) return link;
    link.game_id = normalize_modl_game_id(host);

    // Find the `url=` query parameter. The query string may begin with '?'
    // (canonical) or '&' (modlhandler tolerates it - some sites serialize
    // modl links without the leading '?').
    const auto qpos = rest.find_first_of("?&");
    if (qpos == std::string::npos) return link;
    const std::string query = rest.substr(qpos + 1);

    std::string raw_value;
    std::size_t p = 0;
    while (p < query.size()) {
        const auto eq = query.find('=', p);
        if (eq == std::string::npos) break;
        const auto amp = query.find('&', eq + 1);
        const auto val_end = (amp == std::string::npos) ? query.size() : amp;
        const std::string key = query.substr(p, eq - p);
        if (key == "url") {
            raw_value = query.substr(eq + 1, val_end - eq - 1);
            break;
        }
        if (amp == std::string::npos) break;
        p = amp + 1;
    }
    if (raw_value.empty()) return link;

    // Cap the raw value BEFORE decoding so a hostile modl://.../?url=<1M chars>
    // does not allocate 1M just to be rejected. %XX triples up to 3 bytes per
    // input char, so 4k decoded => 12k encoded is the worst case.
    constexpr std::size_t kMaxDecoded = 4096;
    if (raw_value.size() > kMaxDecoded * 3) return link;

    std::string decoded = engine::download::percent_decode(raw_value);
    if (decoded.empty() || decoded.size() > kMaxDecoded) return link;
    // The decoded https:// URL may itself carry a #fragment (when the
    // originating site percent-encoded the fragment as %23). We never
    // want it to bleed into the direct_url we fetch or log.
    if (const auto h = decoded.find('#'); h != std::string::npos)
        decoded.resize(h);
    // Case-insensitive on the scheme (RFC 3986 says schemes are ASCII
    // case-insensitive; HTTPS:// is valid even if every site emits lower).
    if (!istarts_with_ascii(decoded, "https://")) return link;
    link.direct_url = std::move(decoded);
    return link;
}

Router::DerivedSource Router::derive_source_from_direct_url(
    const std::string& direct_url) {
    DerivedSource out;
    if (direct_url.empty()) return out;
    out.page_url = direct_url;

    // Extract host: between "://" and first '/', '?', '#'. Lowercase ASCII.
    const auto scheme_end = direct_url.find("://");
    if (scheme_end == std::string::npos) return out;
    const auto host_start = scheme_end + 3;
    const auto host_end =
        direct_url.find_first_of("/?#", host_start);
    std::string host = direct_url.substr(
        host_start,
        (host_end == std::string::npos) ? std::string::npos
                                        : host_end - host_start);
    for (auto& c : host)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    // Strip an optional "www." prefix so a CMS-fronted mirror is treated the
    // same as the canonical host.
    if (host.rfind("www.", 0) == 0) host = host.substr(4);
    // Strip an explicit port so a URL like https://mod.pub:443/x still
    // matches the canonical host (the path/query bounds above do not
    // include ':', so a port would otherwise become part of the host).
    if (const auto colon = host.find(':'); colon != std::string::npos)
        host.resize(colon);

    // mod.pub (with or without www, case-insensitive) is the only host the
    // modl flow currently knows how to attribute to a real source. Anything
    // else falls through to manual - the URL still downloads, but no
    // dedicated provider panel is shown (single-source rule fqf5). Exact
    // host match on purpose: "mod.pub.evil.com" must not match.
    if (host == "mod.pub") {
        out.source_type = "modpub";
    }
    return out;
}

} // namespace engine::Source
