#include "engine/source/workshop/remote_cache.h"

#include <curl/curl.h>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace engine {

// libcurl write callback
static size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ss = static_cast<std::string*>(userdata);
    ss->append(ptr, size * nmemb);
    return size * nmemb;
}

RemoteCache::RemoteCache(const std::string& url,
                         const std::string& cache_path,
                         const std::string& bundled_path,
                         std::chrono::seconds ttl,
                         const std::string& user_agent)
    : url_(url)
    , cache_path_(cache_path)
    , bundled_path_(bundled_path)
    , ttl_(ttl)
    , user_agent_(user_agent) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

RemoteCache::~RemoteCache() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (data_ && free_fn_) free_fn_(data_);
    data_ = nullptr;
}

void RemoteCache::set_parse(std::function<void*(const std::string&)> fn) {
    parse_fn_ = std::move(fn);
}

void RemoteCache::set_free(std::function<void(void*)> fn) {
    free_fn_ = std::move(fn);
}

bool RemoteCache::is_fresh() const {
    if (!fs::exists(cache_path_)) return false;
    auto mtime = fs::last_write_time(cache_path_);
    auto age = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
        - std::chrono::file_clock::to_sys(mtime).time_since_epoch());
    return age <= ttl_;
}

void* RemoteCache::get() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (data_) return data_;

    // Try fresh cache
    if (is_fresh()) {
        data_ = try_cache();
        if (data_) return data_;
    }

    // Try network fetch
    data_ = try_fetch();
    if (data_) return data_;

    // Try stale cache
    data_ = try_cache();
    if (data_) return data_;

    // Try bundled fallback
    data_ = try_bundled();
    return data_;
}

std::optional<bool> RemoteCache::fetch_background() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (is_fresh()) return std::nullopt;

    void* fetched = try_fetch();
    if (fetched) {
        if (data_ && free_fn_) free_fn_(data_);
        data_ = fetched;
        return true;
    }
    return false;
}

void RemoteCache::invalidate() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (data_ && free_fn_) free_fn_(data_);
    data_ = nullptr;
}

void* RemoteCache::try_fetch() {
    CURL* curl = curl_easy_init();
    if (!curl) return nullptr;

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url_.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent_.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code != 200) return nullptr;
    if (response.empty()) return nullptr;

    // Save to cache
    fs::create_directories(fs::path(cache_path_).parent_path());
    std::ofstream ofs(cache_path_);
    if (ofs.is_open()) {
        ofs << response;
    }

    if (!parse_fn_) return nullptr;
    return parse_fn_(response);
}

void* RemoteCache::try_cache() {
    if (!fs::exists(cache_path_)) return nullptr;

    std::ifstream ifs(cache_path_);
    if (!ifs.is_open()) return nullptr;

    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    if (content.empty()) return nullptr;

    if (!parse_fn_) return nullptr;
    return parse_fn_(content);
}

void* RemoteCache::try_bundled() {
    if (!fs::exists(bundled_path_)) return nullptr;

    std::ifstream ifs(bundled_path_);
    if (!ifs.is_open()) return nullptr;

    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    if (content.empty()) return nullptr;

    if (!parse_fn_) return nullptr;
    return parse_fn_(content);
}

}  // namespace engine
