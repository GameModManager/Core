#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine {

struct ConflictEntry {
    std::string mod_id;
    uint32_t priority;
};

class ConflictIndex {
public:
    void add_file(const std::string& relative_path,
                  const std::string& mod_id,
                  uint32_t priority);
    void remove_mod(const std::string& mod_id);

    [[nodiscard]] const std::vector<ConflictEntry>& entries_for(
        const std::string& relative_path) const;

    [[nodiscard]] std::string winner(const std::string& relative_path) const;

    [[nodiscard]] const std::unordered_map<std::string,
        std::vector<ConflictEntry>>& all() const { return index_; }

private:
    std::unordered_map<std::string, std::vector<ConflictEntry>> index_;
};

}  // namespace engine
