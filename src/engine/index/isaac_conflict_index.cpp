#include "engine/index/isaac_conflict_index.h"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <optional>
#include <sqlite3.h>
#include <sstream>

namespace fs = std::filesystem;

namespace engine {

void IsaacConflictIndex::set_mods_path(const std::string& mods_path) {
    mods_path_ = mods_path;
}

void IsaacConflictIndex::set_conflict_extensions(
        const std::unordered_set<std::string>& extensions) {
    conflict_extensions_ = extensions;
}

void IsaacConflictIndex::set_ignored_files(
        const std::unordered_set<std::string>& ignored) {
    ignored_files_ = ignored;
}

void IsaacConflictIndex::scan(const std::string& db_path) {
    clear();
    ensure_schema(db_path);

    if (!fs::exists(mods_path_) || !fs::is_directory(mods_path_)) return;

    // Enumerate mod folders and assign priorities (alphabetical = default Isaac order)
    std::vector<std::string> mod_folders;
    for (const auto& entry : fs::directory_iterator(mods_path_)) {
        if (entry.is_directory()) {
            mod_folders.push_back(entry.path().filename().string());
        }
    }
    std::sort(mod_folders.begin(), mod_folders.end());

    for (int i = 0; i < static_cast<int>(mod_folders.size()); i++) {
        priorities_[mod_folders[i]] = i;
    }

    // Scan each mod folder
    for (const auto& folder : mod_folders) {
        fs::path mod_path = fs::path(mods_path_) / folder;

        // Quick token check — skip full walk if unchanged
        std::string token = quick_token(mod_path.string());
        auto cached = load_cache(db_path, folder);
        if (cached && cached->token == token) {
            // Use cached file list
            mod_files_[folder] = {};
            std::istringstream ss(cached->files_json);
            std::string file;
            while (std::getline(ss, file, '\n')) {
                if (!file.empty()) {
                    mod_files_[folder].insert(file);
                }
            }
        } else {
            // Full walk
            auto files = walk_mod(mod_path.string());
            mod_files_[folder] = files;

            // Save to cache
            std::vector<std::string> sorted_files(files.begin(), files.end());
            std::sort(sorted_files.begin(), sorted_files.end());
            save_cache(db_path, folder, fingerprint_folder(mod_path.string()),
                       sorted_files, token);
        }
    }

    // Build conflict index
    for (const auto& [folder, files] : mod_files_) {
        int priority = priorities_.count(folder) ? priorities_[folder] : 999999;
        for (const auto& rel_path : files) {
            conflicts_[rel_path].emplace_back(folder, priority);
        }
    }

    // Sort each path's owners by priority (ascending = loads first = wins)
    for (auto& [path, owners] : conflicts_) {
        std::sort(owners.begin(), owners.end(),
                  [](const auto& a, const auto& b) { return a.second < b.second; });
    }
}

void IsaacConflictIndex::rescan_mod(const std::string& mod_folder,
                                     const std::string& db_path) {
    remove_mod(mod_folder);

    fs::path mod_path = fs::path(mods_path_) / mod_folder;
    if (!fs::exists(mod_path) || !fs::is_directory(mod_path)) return;

    auto files = walk_mod(mod_path.string());
    mod_files_[mod_folder] = files;

    std::string token = quick_token(mod_path.string());
    std::vector<std::string> sorted_files(files.begin(), files.end());
    std::sort(sorted_files.begin(), sorted_files.end());
    save_cache(db_path, mod_folder, fingerprint_folder(mod_path.string()),
               sorted_files, token);

    int priority = priorities_.count(mod_folder) ? priorities_[mod_folder] : 999999;
    for (const auto& rel_path : files) {
        conflicts_[rel_path].emplace_back(mod_folder, priority);
    }
    for (auto& [path, owners] : conflicts_) {
        std::sort(owners.begin(), owners.end(),
                  [](const auto& a, const auto& b) { return a.second < b.second; });
    }
}

void IsaacConflictIndex::remove_mod(const std::string& mod_folder) {
    mod_files_.erase(mod_folder);
    priorities_.erase(mod_folder);

    // Rebuild conflicts from scratch
    ConflictMap rebuilt;
    for (const auto& [folder, files] : mod_files_) {
        int priority = priorities_.count(folder) ? priorities_[folder] : 999999;
        for (const auto& rel_path : files) {
            rebuilt[rel_path].emplace_back(folder, priority);
        }
    }
    for (auto& [path, owners] : rebuilt) {
        std::sort(owners.begin(), owners.end(),
                  [](const auto& a, const auto& b) { return a.second < b.second; });
    }
    conflicts_ = std::move(rebuilt);
}

std::vector<std::pair<std::string, int>>
IsaacConflictIndex::owners_of(const std::string& relative_path) const {
    auto it = conflicts_.find(relative_path);
    if (it == conflicts_.end()) return {};
    return it->second;
}

bool IsaacConflictIndex::has_conflict(const std::string& relative_path) const {
    auto it = conflicts_.find(relative_path);
    return it != conflicts_.end() && it->second.size() > 1;
}

std::string IsaacConflictIndex::winner_of(const std::string& relative_path) const {
    auto it = conflicts_.find(relative_path);
    if (it == conflicts_.end() || it->second.empty()) return {};
    return it->second.front().first;
}

std::unordered_set<std::string>
IsaacConflictIndex::files_for_mod(const std::string& mod_folder) const {
    auto it = mod_files_.find(mod_folder);
    if (it == mod_files_.end()) return {};
    return it->second;
}

void IsaacConflictIndex::clear() {
    mod_files_.clear();
    conflicts_.clear();
    priorities_.clear();
}

// -- Private helpers --

std::unordered_set<std::string>
IsaacConflictIndex::walk_mod(const std::string& mod_path) const {
    std::unordered_set<std::string> files;
    fs::path mod_dir(mod_path);

    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(mod_dir, ec);
         it != fs::recursive_directory_iterator(); ++it) {
        if (!it->is_regular_file()) continue;

        auto fname = it->path().filename().string();
        if (ignored_files_.count(fname)) continue;

        // Skip ignored directories
        bool skip = false;
        fs::path parent = it->path().parent_path();
        while (parent != mod_dir) {
            if (ignored_files_.count(parent.filename().string())) {
                skip = true;
                break;
            }
            parent = parent.parent_path();
        }
        if (skip) continue;

        // Check extension
        auto ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (!conflict_extensions_.count(ext)) continue;

        // Skip top-level files (no directory component in relative path)
        auto rel = fs::relative(it->path(), mod_dir).string();
        if (rel.find('/') == std::string::npos &&
            rel.find('\\') == std::string::npos) continue;

        files.insert(rel);
    }

    return files;
}

std::string IsaacConflictIndex::fingerprint_folder(const std::string& mod_path) const {
    std::hash<std::string> hasher;
    size_t h = 0;

    std::error_code ec;
    auto ftime = fs::last_write_time(mod_path, ec);
    auto mtime = std::chrono::duration_cast<std::chrono::seconds>(
        ftime.time_since_epoch()).count();
    h ^= hasher(std::to_string(mtime)) + 0x9e3779b9 + (h << 6) + (h >> 2);

    // Include conflict-relevant files with their mtimes
    fs::path mod_dir(mod_path);
    for (auto it = fs::recursive_directory_iterator(mod_dir, ec);
         it != fs::recursive_directory_iterator(); ++it) {
        if (!it->is_regular_file()) continue;

        auto fname = it->path().filename().string();
        if (ignored_files_.count(fname)) continue;

        auto ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (!conflict_extensions_.count(ext)) continue;

        auto rel = fs::relative(it->path(), mod_dir).string();
        h ^= hasher(rel) + 0x9e3779b9 + (h << 6) + (h >> 2);

        auto file_mtime = fs::last_write_time(it->path(), ec);
        auto file_time = std::chrono::duration_cast<std::chrono::seconds>(
            file_mtime.time_since_epoch()).count();
        h ^= hasher(std::to_string(file_time)) + 0x9e3779b9 + (h << 6) + (h >> 2);
    }

    char buf[32];
    snprintf(buf, sizeof(buf), "%016zx", h);
    return std::string(buf);
}

std::string IsaacConflictIndex::quick_token(const std::string& mod_path) const {
    std::hash<std::string> hasher;
    size_t h = 0;

    std::error_code ec;
    auto mtime = fs::last_write_time(mod_path, ec);
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(
        mtime.time_since_epoch()).count();
    h ^= hasher(std::to_string(secs)) + 0x9e3779b9 + (h << 6) + (h >> 2);

    // Sorted top-level entries with their mtimes
    std::vector<std::string> entries;
    for (const auto& entry : fs::directory_iterator(mod_path, ec)) {
        auto name = entry.path().filename().string();
        if (ignored_files_.count(name)) continue;
        entries.push_back(name);
    }
    std::sort(entries.begin(), entries.end());

    for (const auto& name : entries) {
        h ^= hasher(name) + 0x9e3779b9 + (h << 6) + (h >> 2);
        auto entry_mtime = fs::last_write_time(fs::path(mod_path) / name, ec);
        auto entry_secs = std::chrono::duration_cast<std::chrono::seconds>(
            entry_mtime.time_since_epoch()).count();
        h ^= hasher(std::to_string(entry_secs)) + 0x9e3779b9 + (h << 6) + (h >> 2);
    }

    char buf[32];
    snprintf(buf, sizeof(buf), "%016zx", h);
    return std::string(buf);
}

// -- SQLite cache helpers --

void IsaacConflictIndex::ensure_schema(const std::string& db_path) const {
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) return;

    sqlite3_exec(db, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS isaac_mod_fingerprints ("
        "  folder TEXT PRIMARY KEY,"
        "  fingerprint TEXT NOT NULL,"
        "  files_json TEXT NOT NULL DEFAULT '',"
        "  token TEXT NOT NULL DEFAULT ''"
        ")",
        nullptr, nullptr, nullptr);

    sqlite3_close(db);
}

std::optional<IsaacConflictIndex::CachedFingerprint>
IsaacConflictIndex::load_cache(const std::string& db_path,
                                const std::string& mod_folder) const {
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) return std::nullopt;

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT fingerprint, files_json, token "
                      "FROM isaac_mod_fingerprints WHERE folder = ?";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return std::nullopt;
    }
    sqlite3_bind_text(stmt, 1, mod_folder.c_str(), -1, SQLITE_STATIC);

    CachedFingerprint result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        result.fingerprint = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        result.files_json = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        result.token = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return result;
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return std::nullopt;
}

void IsaacConflictIndex::save_cache(const std::string& db_path,
                                     const std::string& mod_folder,
                                     const std::string& fingerprint,
                                     const std::vector<std::string>& files,
                                     const std::string& token) const {
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) return;

    std::string files_json;
    for (size_t i = 0; i < files.size(); i++) {
        if (i > 0) files_json += '\n';
        files_json += files[i];
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT OR REPLACE INTO isaac_mod_fingerprints "
                      "(folder, fingerprint, files_json, token) "
                      "VALUES (?, ?, ?, ?)";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, mod_folder.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, fingerprint.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, files_json.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 4, token.c_str(), -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    sqlite3_close(db);
}

void IsaacConflictIndex::delete_cache(const std::string& db_path,
                                       const std::string& mod_folder) const {
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) return;

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "DELETE FROM isaac_mod_fingerprints WHERE folder = ?";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, mod_folder.c_str(), -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    sqlite3_close(db);
}

}  // namespace engine
