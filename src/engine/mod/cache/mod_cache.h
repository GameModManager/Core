#pragma once

#include <string>
#include <vector>

struct sqlite3;

namespace engine {

struct CachedMod {
    std::string id;
    std::string name;
    std::string version;
    std::string state;
    std::string path;
    int priority = 0;
    bool enabled = true;
};

class ModCache {
public:
    explicit ModCache(const std::string& db_path);
    ~ModCache();

    bool open();
    void close();

    bool add_mod(const CachedMod& mod);
    bool update_mod(const CachedMod& mod);
    bool remove_mod(const std::string& id);
    bool set_enabled(const std::string& id, bool enabled);
    bool set_priority(const std::string& id, int priority);

    CachedMod get_mod(const std::string& id) const;
    std::vector<CachedMod> get_all_mods() const;
    std::vector<CachedMod> get_enabled_mods() const;

    bool is_open() const { return db_ != nullptr; }

private:
    void create_tables();

    sqlite3* db_ = nullptr;
    std::string db_path_;
};

}  // namespace engine
