#include "engine/model/profile.h"

#include <algorithm>

namespace engine {

void Profile::add_mod(const std::string& mod_id, bool enabled) {
    remove_mod(mod_id);
    mods_.push_back({mod_id, enabled});
}

void Profile::remove_mod(const std::string& mod_id) {
    mods_.erase(
        std::remove_if(mods_.begin(), mods_.end(),
                        [&](const ProfileMod& m) { return m.mod_id == mod_id; }),
        mods_.end());
}

void Profile::set_enabled(const std::string& mod_id, bool enabled) {
    for (auto& m : mods_) {
        if (m.mod_id == mod_id) {
            m.enabled = enabled;
            return;
        }
    }
}

void Profile::move_mod(const std::string& mod_id, uint32_t new_position) {
    auto it = std::find_if(mods_.begin(), mods_.end(),
                           [&](const ProfileMod& m) { return m.mod_id == mod_id; });
    if (it == mods_.end()) return;

    auto obj = std::move(*it);
    mods_.erase(it);

    if (new_position >= mods_.size()) {
        mods_.push_back(std::move(obj));
    } else {
        mods_.insert(mods_.begin() + new_position, std::move(obj));
    }
}

uint32_t Profile::priority_of(const std::string& mod_id) const {
    for (uint32_t i = 0; i < mods_.size(); ++i) {
        if (mods_[i].mod_id == mod_id) return i;
    }
    return 0;
}

std::vector<std::string> Profile::enabled_in_order() const {
    std::vector<std::string> result;
    for (const auto& m : mods_) {
        if (m.enabled) result.push_back(m.mod_id);
    }
    return result;
}

}  // namespace engine
