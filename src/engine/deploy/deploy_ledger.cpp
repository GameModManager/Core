#include "engine/deploy/deploy_ledger.h"

namespace engine {

void DeployLedger::record_deploy(const std::string& relative_path,
                                  const std::string& mod_id,
                                  uint32_t priority) {
    DeployEntry entry;
    entry.relative_path = relative_path;
    entry.mod_id = mod_id;
    entry.priority = priority;
    entry.deployed = true;
    ledger_[relative_path] = entry;
}

void DeployLedger::record_remove(const std::string& relative_path) {
    ledger_.erase(relative_path);
}

bool DeployLedger::is_deployed(const std::string& relative_path) const {
    auto it = ledger_.find(relative_path);
    return it != ledger_.end() && it->second.deployed;
}

const DeployEntry* DeployLedger::find(const std::string& relative_path) const {
    auto it = ledger_.find(relative_path);
    return it != ledger_.end() ? &it->second : nullptr;
}

std::vector<std::string> DeployLedger::diff(
    const std::unordered_map<std::string, std::string>& new_winners) const {
    std::vector<std::string> changed;

    // Check existing deployments - find paths where winner changed
    for (const auto& [path, entry] : ledger_) {
        auto it = new_winners.find(path);
        if (it == new_winners.end()) {
            // Path no longer has a winner - needs removal
            changed.push_back(path);
        } else if (it->second != entry.mod_id) {
            // Winner changed - needs redeployment
            changed.push_back(path);
        }
    }

    // Check new winners - find paths not in ledger
    for (const auto& [path, mod_id] : new_winners) {
        if (ledger_.find(path) == ledger_.end()) {
            // New path - needs deployment
            changed.push_back(path);
        }
    }

    return changed;
}

void DeployLedger::clear() {
    ledger_.clear();
}

}  // namespace engine
