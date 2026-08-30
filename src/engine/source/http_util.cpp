#include "engine/source/http_util.h"

#include <cctype>

namespace engine::Source::Http {

std::string encode_url_path(const std::string& url) {
    const size_t scheme = url.find("://");
    size_t path_start = std::string::npos;
    if (scheme != std::string::npos)
        path_start = url.find('/', scheme + 3);
    if (path_start == std::string::npos) return url;

    size_t query_start = url.find('?', path_start);
    const size_t path_end = (query_start == std::string::npos) ? url.size() : query_start;

    auto is_hex = [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    };
    const char* hex = "0123456789ABCDEF";

    std::string out = url.substr(0, path_start + 1);
    for (size_t i = path_start + 1; i < path_end; ++i) {
        unsigned char c = static_cast<unsigned char>(url[i]);
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~' || c == '/') {
            out += static_cast<char>(c);
        } else if (c == '%' && i + 2 < path_end && is_hex(url[i + 1]) && is_hex(url[i + 2])) {
            out += c;
            out += url[i + 1];
            out += url[i + 2];
            i += 2;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    if (query_start != std::string::npos) out += url.substr(query_start);
    return out;
}

} // namespace engine::Source::Http
