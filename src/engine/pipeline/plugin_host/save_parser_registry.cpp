#include "engine/pipeline/plugin_host/save_parser_registry.h"

#include <algorithm>
#include <mutex>

namespace engine {

SaveParserRegistry& SaveParserRegistry::instance() {
    static SaveParserRegistry inst;
    return inst;
}

void SaveParserRegistry::register_parser(std::string game_id, int priority,
                                         SaveParserFn fn, void* user_data,
                                         std::string plugin_path) {
    if (!fn) return;
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.push_back(SaveParserEntry{std::move(game_id), std::move(fn),
                                       priority, user_data,
                                       std::move(plugin_path)});
}

void SaveParserRegistry::clear_plugin(const std::string& plugin_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(
        std::remove_if(entries_.begin(), entries_.end(),
                       [&](const SaveParserEntry& e) {
                           return e.plugin_path == plugin_path;
                       }),
        entries_.end());
}

void SaveParserRegistry::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
}

bool SaveParserRegistry::has_parser(const std::string& game_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::any_of(entries_.begin(), entries_.end(),
                       [&](const SaveParserEntry& e) {
                           return e.game_id == game_id;
                       });
}

std::optional<SaveGame> SaveParserRegistry::parse_save(
    const std::filesystem::path& path, const std::string& game_id) const {
    // Resolve the highest-priority parser under the lock, then release it
    // before invoking the (potentially slow, plugin-owned) parser so concurrent
    // scans don't serialize on the registry mutex.
    SaveParserFn fn;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const SaveParserEntry* best = nullptr;
        for (const auto& e : entries_) {
            if (e.game_id != game_id) continue;
            if (!best || e.priority > best->priority) {
                best = &e;
            }
        }
        if (!best) return std::nullopt;
        fn = best->fn;
    }
    return fn(path, game_id);
}

}  // namespace engine
