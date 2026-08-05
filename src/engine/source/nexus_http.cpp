#include "engine/source/nexus_http.h"

#include <cctype>
#include <cstddef>

namespace engine {

namespace {

static size_t write_to_string(void* ptr, size_t size, size_t nmemb, void* stream) {
    auto* str = static_cast<std::string*>(stream);
    str->append(static_cast<const char*>(ptr), size * nmemb);
    return size * nmemb;
}

static size_t header_callback(void* ptr, size_t size, size_t nmemb, void* user_data) {
    auto* headers = static_cast<std::string*>(user_data);
    headers->append(static_cast<const char*>(ptr), size * nmemb);
    return size * nmemb;
}

} // namespace

bool nexus_http_request(const std::string& url,
                        const std::string& post_body,
                        std::string& response_body,
                        long& http_code,
                        curl_slist* headers,
                        std::string* response_headers,
                        long timeout_seconds) {
    auto* curl = curl_easy_init();
    if (!curl) return false;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                     "GameModManager/0.1 (Nexus Provider)");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);

    // Nexus API AUP (help.nexusmods.com/article/114) requires apps to identify
    // themselves via application-name/application-version headers. They are
    // appended on top of any caller-supplied headers (apikey etc. survive).
    curl_slist* effective = nullptr;
    for (curl_slist* h = headers; h; h = h->next)
        effective = curl_slist_append(effective, h->data);
    effective = curl_slist_append(effective, "application-name: GameModManager");
    effective = curl_slist_append(effective, "application-version: " VERSION);
    if (effective)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, effective);

    if (response_headers) {
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, response_headers);
    }

    if (!post_body.empty()) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, post_body.size());
    }

    CURLcode res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(effective);
    curl_easy_cleanup(curl);

    return (res == CURLE_OK);
}

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

} // namespace engine
