#include "engine/index/conflict_index.h"

#include <algorithm>

namespace engine {

void ConflictIndex::add_file(const std::string& relative_path,
                             const std::string& mod_id,
                             uint32_t priority) {
    auto& entries = index_[relative_path];

    // Remove existing entry for this mod if present
    entries.erase(
        std::remove_if(entries.begin(), entries.end(),
                        [&](const ConflictEntry& e) { return e.mod_id == mod_id; }),
        entries.end());

    entries.push_back({mod_id, priority});

    // Sort: higher priority number wins (later in list = higher priority)
    std::sort(entries.begin(), entries.end(),
              [](const ConflictEntry& a, const ConflictEntry& b) {
                  return a.priority < b.priority;
              });
}

void ConflictIndex::remove_mod(const std::string& mod_id) {
    for (auto& [path, entries] : index_) {
        entries.erase(
            std::remove_if(entries.begin(), entries.end(),
                            [&](const ConflictEntry& e) { return e.mod_id == mod_id; }),
            entries.end());
    }
}

const std::vector<ConflictEntry>& ConflictIndex::entries_for(
    const std::string& relative_path) const {
    static const std::vector<ConflictEntry> empty;
    auto it = index_.find(relative_path);
    return it != index_.end() ? it->second : empty;
}

std::string ConflictIndex::winner(const std::string& relative_path) const {
    auto& entries = entries_for(relative_path);
    if (entries.empty()) return {};
    return entries.back().mod_id;
}

}  // namespace engine
