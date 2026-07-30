#include "engine/cache/mod_cache.h"
#include "engine/log/logger.h"

#include <sqlite3.h>

namespace engine {

ModCache::ModCache(const std::string& db_path)
    : db_path_(db_path) {}

ModCache::~ModCache() {
    close();
}

bool ModCache::open() {
    int rc = sqlite3_open(db_path_.c_str(), &db_);
    if (rc != SQLITE_OK) {
        Logger::instance().error("Failed to open mod cache: " + db_path_);
        return false;
    }
    create_tables();
    Logger::instance().debug("Mod cache opened: " + db_path_);
    return true;
}

void ModCache::close() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

void ModCache::create_tables() {
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS mods (
            id TEXT PRIMARY KEY,
            name TEXT NOT NULL,
            version TEXT DEFAULT '',
            state TEXT DEFAULT 'downloaded',
            path TEXT DEFAULT '',
            priority INTEGER DEFAULT 0,
            enabled INTEGER DEFAULT 1
        );
    )";

    char* err = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        Logger::instance().error(std::string("Failed to create tables: ") + (err ? err : "unknown"));
        sqlite3_free(err);
    }
}

bool ModCache::add_mod(const CachedMod& mod) {
    const char* sql = "INSERT INTO mods (id, name, version, state, path, priority, enabled) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?);";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, mod.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, mod.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, mod.version.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, mod.state.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, mod.path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 6, mod.priority);
    sqlite3_bind_int(stmt, 7, mod.enabled ? 1 : 0);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool ModCache::update_mod(const CachedMod& mod) {
    const char* sql = "UPDATE mods SET name=?, version=?, state=?, path=?, priority=?, enabled=? "
                      "WHERE id=?;";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, mod.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, mod.version.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, mod.state.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, mod.path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, mod.priority);
    sqlite3_bind_int(stmt, 6, mod.enabled ? 1 : 0);
    sqlite3_bind_text(stmt, 7, mod.id.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool ModCache::remove_mod(const std::string& id) {
    const char* sql = "DELETE FROM mods WHERE id=?;";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool ModCache::set_enabled(const std::string& id, bool enabled) {
    const char* sql = "UPDATE mods SET enabled=? WHERE id=?;";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_int(stmt, 1, enabled ? 1 : 0);
    sqlite3_bind_text(stmt, 2, id.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool ModCache::set_priority(const std::string& id, int priority) {
    const char* sql = "UPDATE mods SET priority=? WHERE id=?;";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_int(stmt, 1, priority);
    sqlite3_bind_text(stmt, 2, id.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

CachedMod ModCache::get_mod(const std::string& id) const {
    CachedMod mod;
    const char* sql = "SELECT id, name, version, state, path, priority, enabled FROM mods WHERE id=?;";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return mod;

    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        mod.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        mod.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        mod.version = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        mod.state = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        mod.path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        mod.priority = sqlite3_column_int(stmt, 5);
        mod.enabled = sqlite3_column_int(stmt, 6) != 0;
    }

    sqlite3_finalize(stmt);
    return mod;
}

std::vector<CachedMod> ModCache::get_all_mods() const {
    std::vector<CachedMod> mods;
    const char* sql = "SELECT id, name, version, state, path, priority, enabled FROM mods "
                      "ORDER BY priority ASC;";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return mods;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        CachedMod mod;
        mod.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        mod.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        mod.version = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        mod.state = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        mod.path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        mod.priority = sqlite3_column_int(stmt, 5);
        mod.enabled = sqlite3_column_int(stmt, 6) != 0;
        mods.push_back(std::move(mod));
    }

    sqlite3_finalize(stmt);
    return mods;
}

std::vector<CachedMod> ModCache::get_enabled_mods() const {
    std::vector<CachedMod> mods;
    const char* sql = "SELECT id, name, version, state, path, priority, enabled FROM mods "
                      "WHERE enabled=1 ORDER BY priority ASC;";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return mods;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        CachedMod mod;
        mod.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        mod.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        mod.version = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        mod.state = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        mod.path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        mod.priority = sqlite3_column_int(stmt, 5);
        mod.enabled = sqlite3_column_int(stmt, 6) != 0;
        mods.push_back(std::move(mod));
    }

    sqlite3_finalize(stmt);
    return mods;
}

}  // namespace engine
