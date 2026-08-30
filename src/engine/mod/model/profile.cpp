#include "engine/mod/model/profile.h"

#include <algorithm>

namespace engine {

ProfileModel::ProfileModel() {
    ensure_overwrite_pinned();
}

void ProfileModel::add_mod(const std::string& mod_id, bool enabled) {
    if (is_overwrite(mod_id)) return;  // cannot add Overwrite manually
    remove_mod(mod_id);
    // Insert before the Overwrite entry (which is always last)
    auto it = std::find_if(mods_.rbegin(), mods_.rend(),
                           [](const ProfileMod& m) { return false; });
    // Always insert before Overwrite
    auto ow = std::find_if(mods_.begin(), mods_.end(),
                           [](const ProfileMod& m) { return m.mod_id == kProfileOverwriteId; });
    if (ow != mods_.end()) {
        mods_.insert(ow, {mod_id, enabled});
    } else {
        mods_.push_back({mod_id, enabled});
    }
}

void ProfileModel::remove_mod(const std::string& mod_id) {
    if (is_overwrite(mod_id)) return;  // cannot remove Overwrite
    mods_.erase(
        std::remove_if(mods_.begin(), mods_.end(),
                        [&](const ProfileMod& m) { return m.mod_id == mod_id; }),
        mods_.end());
}

void ProfileModel::set_enabled(const std::string& mod_id, bool enabled) {
    if (is_overwrite(mod_id)) return;  // Overwrite is always enabled
    for (auto& m : mods_) {
        if (m.mod_id == mod_id) {
            m.enabled = enabled;
            return;
        }
    }
}

void ProfileModel::move_mod(const std::string& mod_id, uint32_t new_position) {
    if (is_overwrite(mod_id)) return;  // cannot move Overwrite

    auto it = std::find_if(mods_.begin(), mods_.end(),
                           [&](const ProfileMod& m) { return m.mod_id == mod_id; });
    if (it == mods_.end()) return;

    auto obj = std::move(*it);
    mods_.erase(it);

    // Clamp so Overwrite (last) stays last
    int max_pos = static_cast<int>(mods_.size());
    if (new_position >= static_cast<uint32_t>(max_pos)) {
        // Insert before Overwrite
        auto ow = std::find_if(mods_.begin(), mods_.end(),
                               [](const ProfileMod& m) { return m.mod_id == kProfileOverwriteId; });
        if (ow != mods_.end()) {
            mods_.insert(ow, std::move(obj));
        } else {
            mods_.push_back(std::move(obj));
        }
    } else {
        mods_.insert(mods_.begin() + new_position, std::move(obj));
    }
}

uint32_t ProfileModel::priority_of(const std::string& mod_id) const {
    for (uint32_t i = 0; i < mods_.size(); ++i) {
        if (mods_[i].mod_id == mod_id) return i;
    }
    return 0;
}

std::vector<std::string> ProfileModel::enabled_in_order() const {
    std::vector<std::string> result;
    for (const auto& m : mods_) {
        if (m.enabled) result.push_back(m.mod_id);
    }
    return result;
}

void ProfileModel::ensure_overwrite_pinned() {
    for (const auto& m : mods_) {
        if (m.mod_id == kProfileOverwriteId) return;  // already present
    }
    mods_.push_back({kProfileOverwriteId, true});
}

bool ProfileModel::is_overwrite(const std::string& mod_id) const {
    return mod_id == kProfileOverwriteId;
}

int ProfileModel::overwrite_index() const {
    for (int i = 0; i < static_cast<int>(mods_.size()); ++i) {
        if (mods_[i].mod_id == kProfileOverwriteId) return i;
    }
    return -1;
}

}  // namespace engine
