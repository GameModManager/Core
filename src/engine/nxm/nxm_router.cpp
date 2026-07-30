#include "engine/nxm/nxm_router.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace engine {

NxmLink NxmRouter::parse(const std::string& url) {
    NxmLink link;
    link.full_url = url;

    // Expected: nxm://<domain>/mods/<mod_id>/files/<file_id>?key=...&expire=...&user_id=...
    // Or:       nxm://<domain>/mods/<mod_id>/files/<file_id>&key=...
    // The domain is everything after "nxm://" and before the first '/'

    const std::string prefix = "nxm://";
    if (url.size() <= prefix.size()) return link;
    if (url.substr(0, prefix.size()) != prefix) return link;

    auto rest = url.substr(prefix.size());

    // Extract domain (up to first '/' or '?')
    auto domain_end = rest.find_first_of("/?");
    if (domain_end == std::string::npos) {
        // Just a domain, no path — e.g. "nxm://isaac"
        link.nexus_domain = rest;
        return link;
    }
    link.nexus_domain = rest.substr(0, domain_end);
    if (link.nexus_domain.empty()) return link;

    auto path = rest.substr(domain_end + 1);

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
            // file_id may have query params appended — strip at '?' or '&'
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

    // Find query string — could start with '?' or be embedded with '&'
    {
        std::string query;
        auto q1 = url.find('?');
        if (q1 != std::string::npos) {
            query = url.substr(q1 + 1);
        } else {
            // nxm links sometimes use & without ? — find "key=" anywhere
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
                else if (key == "expire") {
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

std::string NxmRouter::match_game(
    const std::string& nexus_domain,
    const std::vector<std::pair<std::string, std::string>>& plugin_domains) {
    // plugin_domains: vector of (game_id, nexus_domain) from loaded plugins
    for (const auto& [game_id, domain] : plugin_domains) {
        if (domain == nexus_domain) return game_id;
    }
    return {};
}

}  // namespace engine
