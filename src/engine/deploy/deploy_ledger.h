#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine {

struct DeployEntry {
    std::string relative_path;
    std::string mod_id;
    uint32_t priority = 0;
    bool deployed = false;
};

class DeployLedger {
public:
    void record_deploy(const std::string& relative_path,
                       const std::string& mod_id,
                       uint32_t priority);

    void record_remove(const std::string& relative_path);

    [[nodiscard]] bool is_deployed(const std::string& relative_path) const;

    [[nodiscard]] const DeployEntry* find(const std::string& relative_path) const;

    // Get paths that need redeployment after a priority change
    [[nodiscard]] std::vector<std::string> diff(
        const std::unordered_map<std::string, std::string>& new_winners) const;

    void clear();

    [[nodiscard]] size_t size() const { return ledger_.size(); }

private:
    std::unordered_map<std::string, DeployEntry> ledger_;
};

}  // namespace engine
