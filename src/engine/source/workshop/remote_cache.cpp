#include "engine/source/workshop/remote_cache.h"

#include "engine/network/network_manager.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace engine {

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
    // curl_global_init() used to live here; libcurl refcounts it so
    // Network::Manager already initialises globals when the first Manager
    // is constructed. The init call here is unnecessary and dragged in
    // <curl/curl.h> as a transitive leak - dropped.
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
         - std::chrono::clock_cast<std::chrono::system_clock>(mtime).time_since_epoch());
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
    // Single GET via Network::. Network:: enforces timeout / proxy / log
    // redaction uniformly; the body comes back as a string ready to feed
    // the parser. No raw curl handle here.
    network::Request req;
    req.url = url_;
    req.caller = NET_CALLER;
    req.timeout = std::chrono::seconds(10);
    req.follow_redirect = true;
    if (!user_agent_.empty())
        req.headers.push_back("User-Agent: " + user_agent_);

    auto resp = network::instance().request(req);
    if (!resp.error.empty() || resp.http_code != 200) return nullptr;
    if (resp.body.empty()) return nullptr;

    // Save to cache
    fs::create_directories(fs::path(cache_path_).parent_path());
    std::ofstream ofs(cache_path_);
    if (ofs.is_open()) {
        ofs << resp.body;
    }

    if (!parse_fn_) return nullptr;
    return parse_fn_(resp.body);
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
