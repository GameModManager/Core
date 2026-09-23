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
  if (!fn)
    return;
  std::lock_guard<std::mutex> lock(mutex_);
  entries_.push_back(SaveParserEntry{std::move(game_id), std::move(fn), priority,
                                     user_data, std::move(plugin_path)});
}

void SaveParserRegistry::clear_plugin(const std::string& plugin_path) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto drop_owned = [&](std::vector<SaveParserEntry>& entries) {
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [&](const SaveParserEntry& e) {
                                   return e.plugin_path == plugin_path;
                                 }),
                  entries.end());
  };
  drop_owned(entries_);
  drop_owned(fast_entries_);
}

void SaveParserRegistry::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  entries_.clear();
  fast_entries_.clear();
}

bool SaveParserRegistry::has_parser(const std::string& game_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return std::any_of(entries_.begin(), entries_.end(), [&](const SaveParserEntry& e) {
    return e.game_id == game_id;
  });
}

std::optional<SaveGame>
SaveParserRegistry::parse_save(const std::filesystem::path& path,
                               const std::string& game_id) const {
  // Resolve the highest-priority parser under the lock, then release it
  // before invoking the (potentially slow, plugin-owned) parser so concurrent
  // scans don't serialize on the registry mutex.
  SaveParserFn fn;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const SaveParserEntry* best = nullptr;
    for (const auto& e : entries_) {
      if (e.game_id != game_id)
        continue;
      if (!best || e.priority > best->priority) {
        best = &e;
      }
    }
    if (!best)
      return std::nullopt;
    fn = best->fn;
  }
  return fn(path, game_id);
}

void SaveParserRegistry::register_fast_parser(std::string game_id, int priority,
                                              SaveFastParserFn fn, void* user_data,
                                              std::string plugin_path) {
  if (!fn)
    return;
  std::lock_guard<std::mutex> lock(mutex_);
  fast_entries_.push_back(SaveParserEntry{std::move(game_id), std::move(fn), priority,
                                          user_data, std::move(plugin_path)});
}

bool SaveParserRegistry::has_fast_parser(const std::string& game_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return std::any_of(fast_entries_.begin(), fast_entries_.end(),
                     [&](const SaveParserEntry& e) {
                       return e.game_id == game_id;
                     });
}

std::optional<SaveGame>
SaveParserRegistry::parse_save_fast(const std::filesystem::path& path,
                                    const std::string& game_id) const {
  // Same resolve-then-release shape as parse_save: the fast parser is still
  // plugin-owned code and must not run under the registry mutex.
  SaveParserFn fn;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const SaveParserEntry* best = nullptr;
    for (const auto& e : fast_entries_) {
      if (e.game_id != game_id)
        continue;
      if (!best || e.priority > best->priority) {
        best = &e;
      }
    }
    if (!best)
      return std::nullopt;
    fn = best->fn;
  }
  return fn(path, game_id);
}

}  // namespace engine
