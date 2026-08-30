#include "engine/source/loverslab_auth.h"

#include "engine/core/log/logger.h"
#include "platform/platform.h"

#include <cctype>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace engine {

namespace {
constexpr const char* kCookieName = "loverslab-cookie";

// Cloudflare challenge cookies are bound to the browser that solved the
// challenge (IP + User-Agent + TLS fingerprint). They can never validate from
// libcurl's different fingerprint, and including one makes Cloudflare reject a
// request that would otherwise succeed (verified against a live download: 200
// with the IPS4 cookies alone, 403 once cf_clearance was added). Drop them.
bool is_cloudflare_cookie(const std::string& name) {
    if (name == "cf_clearance") return true;
    return name.size() >= 4 && name.compare(0, 4, "__cf") == 0;
}

std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// Strip one pair of surrounding double quotes (devtools/JSON exports quote
// values: name<TAB>"value").
std::string unquote(std::string t) {
    t = trim(t);
    if (t.size() >= 2 && t.front() == '"' && t.back() == '"')
        return t.substr(1, t.size() - 2);
    return t;
}

} // namespace

std::filesystem::path LoversLabAuth::config_dir() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg)
        return std::filesystem::path(xdg) / "GameModManager";
    return safe_home_dir() / ".config" / "GameModManager";
}

LoversLabAuth::LoversLabAuth()
    : fallback_(config_dir()) {}

LoversLabAuth& LoversLabAuth::instance() {
    static LoversLabAuth inst;
    return inst;
}

void LoversLabAuth::set_keyring(std::unique_ptr<Keyring> keyring) {
    keyring_ = std::move(keyring);
}

Keyring& LoversLabAuth::effective_keyring() const {
    if (keyring_ && keyring_->available())
        return *keyring_;
    return fallback_;
}

bool LoversLabAuth::has_cookie() const {
    try {
        return effective_keyring().has(kCookieName);
    } catch (const std::exception& e) {
        Logger::instance().error("LoversLabAuth: keyring has() failed: " +
                                 std::string(e.what()));
        return false;
    }
}

std::string LoversLabAuth::get_cookie() const {
    try {
        Keyring& kr = effective_keyring();
        if (!kr.available()) {
            Logger::instance().error(
                "LoversLabAuth: no OS keyring; falling back to insecure file storage");
        }
        // Sanitize on read too: a cookie stored before sanitization existed
        // (e.g. pasted multi-line) is fixed without forcing a re-paste.
        return sanitize_cookie(kr.get(kCookieName));
    } catch (const std::exception& e) {
        Logger::instance().error("LoversLabAuth: keyring get() failed: " +
                                 std::string(e.what()));
        return {};
    }
}

void LoversLabAuth::set_cookie(const std::string& cookie) {
    try {
        Keyring& kr = effective_keyring();
        if (!kr.available()) {
            Logger::instance().error(
                "LoversLabAuth: no OS keyring; falling back to insecure file storage");
        }
        if (!kr.set(kCookieName, sanitize_cookie(cookie))) {
            Logger::instance().error(
                "LoversLabAuth: failed to store session cookie in keyring");
        }
    } catch (const std::exception& e) {
        Logger::instance().error("LoversLabAuth: keyring set() failed: " +
                                 std::string(e.what()));
    }
}

void LoversLabAuth::clear_cookie() {
    try {
        effective_keyring().remove(kCookieName);
    } catch (const std::exception& e) {
        Logger::instance().error("LoversLabAuth: keyring remove() failed: " +
                                 std::string(e.what()));
    }
}

std::string LoversLabAuth::redact(const std::string& cookie) {
    if (cookie.empty()) return "<empty>";
    const std::size_t eq = cookie.find('=');
    const std::string name =
        (eq != std::string::npos) ? cookie.substr(0, eq) : "<cookie>";
    return name + "=… (" + std::to_string(cookie.size()) + " bytes)";
}

std::string LoversLabAuth::sanitize_cookie(const std::string& raw) {
    if (raw.empty()) return {};

    auto add_pair = [](std::vector<std::pair<std::string, std::string>>& pairs,
                       std::string name, std::string value) {
        name = unquote(name);
        value = unquote(value);
        if (name.empty() || value.empty()) return;
        if (is_cloudflare_cookie(name)) return;
        for (const auto& p : pairs) {
            if (p.first == name) return;  // first occurrence wins
        }
        pairs.emplace_back(std::move(name), std::move(value));
    };

    std::vector<std::pair<std::string, std::string>> pairs;

    // JSON export form: [{"name": "...", "value": "..."}, ...]. Identified by
    // the literal "name"/"value" keys (a plain cookie header never has them,
    // and base64 values such as "eyJ...==" must not trip the token path).
    const bool json_form = raw.find("\"name\"") != std::string::npos &&
                           raw.find("\"value\"") != std::string::npos;

    if (json_form) {
        // Collect every "key": "value" string pair in document order, then pair
        // each "name" with the next "value" (the JSON array layout). Non-string
        // values (null/true/numbers) are skipped by the string scan.
        std::vector<std::pair<std::string, std::string>> kv;
        std::size_t pos = 0;
        while (true) {
            const std::size_t kq = raw.find('"', pos);
            if (kq == std::string::npos) break;
            const std::size_t kend = raw.find('"', kq + 1);
            if (kend == std::string::npos) break;
            const std::string key = raw.substr(kq + 1, kend - kq - 1);
            const std::size_t colon = raw.find(':', kend);
            if (colon == std::string::npos) break;
            const std::size_t vq = raw.find('"', colon);
            if (vq == std::string::npos) break;
            const std::size_t vend = raw.find('"', vq + 1);
            if (vend == std::string::npos) break;
            kv.emplace_back(key, raw.substr(vq + 1, vend - vq - 1));
            pos = vend + 1;
        }
        std::string pending_name;
        for (const auto& [k, v] : kv) {
            if (k == "name") {
                pending_name = v;
            } else if (k == "value" && !pending_name.empty()) {
                add_pair(pairs, pending_name, v);
                pending_name.clear();
            }
        }
    } else {
        // Token form: name=value pairs separated by ';', newlines or CRLF; the
        // devtools "Request Cookies" block is additionally TAB-separated
        // (name<TAB>"value" per line).
        std::size_t start = 0;
        while (start < raw.size()) {
            const std::size_t sep = raw.find_first_of(";\r\n", start);
            std::string token = raw.substr(
                start, (sep == std::string::npos) ? std::string::npos : sep - start);
            start = (sep == std::string::npos) ? raw.size() : sep + 1;

            const std::size_t eq = token.find('=');
            const std::size_t tab = token.find('\t');
            std::string name, value;
            if (eq != std::string::npos && (tab == std::string::npos || eq < tab)) {
                name = token.substr(0, eq);
                value = token.substr(eq + 1);
            } else if (tab != std::string::npos) {
                name = token.substr(0, tab);
                value = token.substr(tab + 1);
            } else {
                continue;  // no '=' and no tab: not a cookie, skip
            }
            add_pair(pairs, name, value);
        }
    }

    if (pairs.empty()) return {};
    std::string out;
    for (const auto& p : pairs) {
        if (!out.empty()) out += "; ";
        out += p.first + "=" + p.second;
    }
    return out;
}

} // namespace engine
