#pragma once

#include "engine/keyring.h"

#include <filesystem>
#include <memory>
#include <string>

namespace engine {

// Manages LoversLab session-cookie storage. LoversLab has no public API, so
// downloads ride on a browser session: the user copies the site's cookies
// (name=value pairs joined by "; ") and GMM sends them as the Cookie header
// while fetching a ?do=download link. The cookie is a session secret, so it
// lives in the OS keyring (injected via set_keyring(), same backend as the
// Nexus API key); when no OS keyring is available storage falls back to
// FileKeyring (obfuscated file, insecure) with a warning.
class LoversLabAuth {
public:
    static LoversLabAuth& instance();

    bool has_cookie() const;
    std::string get_cookie() const;
    void set_cookie(const std::string& cookie);
    void clear_cookie();

    // Injects the OS-backed keyring. Call once at startup (before first use).
    void set_keyring(std::unique_ptr<Keyring> keyring);

    // Log-safe preview of a cookie for the UI/logs: shows only the first
    // cookie's name and the total size, never any value.
    static std::string redact(const std::string& cookie);

    // Normalizes a pasted cookie blob into a single-line "name=value;
    // name=value; ..." Cookie header. Accepts any of the common export forms:
    //   - the header itself ("a=1; b=2; ..."),
    //   - newline- or CRLF-separated name=value lines,
    //   - the browser devtools "Request Cookies" block (name<TAB>"value" lines),
    //   - a JSON export ([{"name": "...", "value": "..."}, ...]).
    // Surrounding quotes and whitespace are stripped; malformed tokens (no
    // '=') are skipped; Cloudflare challenge cookies (cf_clearance, __cf_*)
    // are dropped because they are bound to the browser's IP/UA/TLS fingerprint
    // and make libcurl's request fail where the rest of the cookie would
    // succeed. This is what set_cookie()/get_cookie() run so the header can
    // never contain the \r/\n that libcurl rejects with CURLE_BAD_FUNCTION_ARGUMENT.
    static std::string sanitize_cookie(const std::string& raw);

    static std::filesystem::path config_dir();

private:
    LoversLabAuth();

    // The backend actually used: the injected keyring when available and
    // reachable, otherwise the file fallback.
    Keyring& effective_keyring() const;

    mutable std::unique_ptr<Keyring> keyring_;
    mutable FileKeyring fallback_;
};

} // namespace engine
