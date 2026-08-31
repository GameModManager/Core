#include "engine/source/workshop/workshop_client.h"

#include <algorithm>
#include <chrono>
#include <curl/curl.h>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <sstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace engine {

static size_t curl_write_string(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

WorkshopClient::WorkshopClient(const std::string& db_path, int rate_limit, int rate_window)
    : db_path_(db_path), rate_limit_(rate_limit), rate_window_(rate_window) {
    ensure_schema();
    load_dead_ids();
}

void WorkshopClient::ensure_schema() {
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path_.c_str(), &db) != SQLITE_OK) return;

    sqlite3_exec(db, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS workshop_items ("
        "  id INTEGER PRIMARY KEY,"
        "  title TEXT DEFAULT '',"
        "  preview_url TEXT DEFAULT '',"
        "  description TEXT DEFAULT '',"
        "  tags TEXT DEFAULT '',"
        "  created_at REAL,"
        "  updated_at REAL,"
        "  status TEXT DEFAULT ''"
        ")",
        nullptr, nullptr, nullptr);

    // Migration: add tags column if missing (pre-tags databases)
    sqlite3_exec(db,
        "ALTER TABLE workshop_items ADD COLUMN tags TEXT DEFAULT ''",
        nullptr, nullptr, nullptr);

    sqlite3_close(db);
}

void WorkshopClient::load_dead_ids() {
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path_.c_str(), &db) != SQLITE_OK) return;

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id FROM workshop_items WHERE status = 'dead'";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            dead_ids_.insert(sqlite3_column_int64(stmt, 0));
        }
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
}

std::optional<WorkshopItem> WorkshopClient::get_details(int64_t workshop_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (dead_ids_.count(workshop_id)) return std::nullopt;

    // Check in-memory cache
    auto it = cache_.find(workshop_id);
    if (it != cache_.end()) return it->second;

    // Check SQLite cache
    auto cached = get_cached(workshop_id);
    if (cached) {
        cache_[workshop_id] = *cached;
        return cached;
    }

    // Rate limit check
    if (!can_request()) return std::nullopt;

    // Fetch from Steam API
    auto item = fetch_from_steam(workshop_id);
    if (item) {
        save_to_cache(*item);
        cache_[workshop_id] = *item;
        request_timestamps_.push_back(
            std::chrono::duration<double>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        return item;
    }

    return std::nullopt;
}

std::optional<WorkshopItem> WorkshopClient::get_cached(int64_t workshop_id) const {
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path_.c_str(), &db) != SQLITE_OK) return std::nullopt;

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, title, preview_url, description, "
                      "tags, created_at, updated_at, status "
                      "FROM workshop_items WHERE id = ?";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return std::nullopt;
    }
    sqlite3_bind_int64(stmt, 1, workshop_id);

    WorkshopItem item;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        item.workshop_id = sqlite3_column_int64(stmt, 0);
        auto text = [](sqlite3_stmt* s, int i) -> std::string {
            const char* p = reinterpret_cast<const char*>(sqlite3_column_text(s, i));
            return p ? p : "";
        };
        item.title = text(stmt, 1);
        item.preview_url = text(stmt, 2);
        item.description = text(stmt, 3);
        // Tags stored as comma-separated string
        auto tags_str = text(stmt, 4);
        if (!tags_str.empty()) {
            std::istringstream ss(tags_str);
            std::string tag;
            while (std::getline(ss, tag, ',')) {
                if (!tag.empty()) item.tags.push_back(tag);
            }
        }
        item.created_at = sqlite3_column_double(stmt, 5);
        item.updated_at = sqlite3_column_double(stmt, 6);
        item.status = text(stmt, 7);
        sqlite3_finalize(stmt);
        sqlite3_close(db);

        if (item.status == "dead") return std::nullopt;
        return item;
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return std::nullopt;
}

std::optional<WorkshopItem> WorkshopClient::fetch_from_steam(int64_t workshop_id) {
    CURL* curl = curl_easy_init();
    if (!curl) return std::nullopt;

    // Build the API URL
    std::string url = "https://api.steampowered.com/ISteamRemoteStorage/"
                      "GetPublishedFileDetails/v1/";

    // POST with form data
    std::string postfields =
        "itemcount=1&publishedfileids[0]=" + std::to_string(workshop_id);

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postfields.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "GameModManager/1.0");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code != 200 || response.empty()) {
        return std::nullopt;
    }

    try {
        auto j = json::parse(response);
        auto& details = j["response"]["publishedfiledetails"];
        if (!details.is_array() || details.empty()) return std::nullopt;

        auto& item = details[0];
        int result = item.value("result", 0);
        if (result == 9) {
            // File not found - mark as dead
            mark_dead(workshop_id);
            return std::nullopt;
        }
        if (result != 1) return std::nullopt;

        WorkshopItem wi;
        wi.workshop_id = workshop_id;
        wi.title = item.value("title", "");
        wi.preview_url = item.value("preview_url", "");
        wi.description = item.value("short_description", "");
        // Parse tags from Steam API response - "tags" is an array of
        // objects with a "tag" string field (e.g. [{"tag":"Lua"}, ...]).
        if (item.contains("tags") && item["tags"].is_array()) {
            for (const auto& t : item["tags"]) {
                if (t.contains("tag") && t["tag"].is_string()) {
                    wi.tags.push_back(t["tag"].get<std::string>());
                }
            }
        }
        wi.created_at = item.value("time_created", 0.0);
        wi.updated_at = item.value("time_updated", 0.0);
        wi.status = "ok";
        return wi;
    } catch (...) {
        return std::nullopt;
    }
}

void WorkshopClient::save_to_cache(const WorkshopItem& item) {
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path_.c_str(), &db) != SQLITE_OK) return;

    // Join tags into comma-separated string
    std::string tags_csv;
    for (size_t i = 0; i < item.tags.size(); ++i) {
        if (i > 0) tags_csv += ',';
        tags_csv += item.tags[i];
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT OR REPLACE INTO workshop_items "
                      "(id, title, preview_url, description, tags, "
                      "created_at, updated_at, status) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?, ?)";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, item.workshop_id);
        sqlite3_bind_text(stmt, 2, item.title.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, item.preview_url.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 4, item.description.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 5, tags_csv.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_double(stmt, 6, item.created_at);
        sqlite3_bind_double(stmt, 7, item.updated_at);
        sqlite3_bind_text(stmt, 8, item.status.c_str(), -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
}

void WorkshopClient::mark_dead(int64_t workshop_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    dead_ids_.insert(workshop_id);

    sqlite3* db = nullptr;
    if (sqlite3_open(db_path_.c_str(), &db) != SQLITE_OK) return;

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT OR REPLACE INTO workshop_items (id, status) VALUES (?, 'dead')";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, workshop_id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
}

bool WorkshopClient::is_dead(int64_t workshop_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dead_ids_.count(workshop_id) > 0;
}

std::vector<int64_t> WorkshopClient::dead_ids() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {dead_ids_.begin(), dead_ids_.end()};
}

std::pair<int, std::optional<double>> WorkshopClient::rate_limit_state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    double now = std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Prune old timestamps
    int count = 0;
    std::optional<double> next_available;
    for (double ts : request_timestamps_) {
        if (ts > now - rate_window_) count++;
    }

    if (count >= rate_limit_ && !request_timestamps_.empty()) {
        double oldest = *std::min_element(request_timestamps_.begin(),
                                           request_timestamps_.end());
        next_available = oldest + rate_window_;
    }

    return {count, next_available};
}

bool WorkshopClient::can_request() const {
    auto [count, _] = rate_limit_state();
    return count < rate_limit_;
}

void WorkshopClient::set_rate_limit(int limit, int window) {
    std::lock_guard<std::mutex> lock(mutex_);
    rate_limit_ = limit > 0 ? limit : rate_limit_;
    rate_window_ = window > 0 ? window : rate_window_;
    // Prune timestamps that fall outside the new window.
    double now = std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    request_timestamps_.erase(
        std::remove_if(request_timestamps_.begin(), request_timestamps_.end(),
                       [&](double ts) { return ts <= now - rate_window_; }),
        request_timestamps_.end());
}

}  // namespace engine
