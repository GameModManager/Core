#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace engine {

// Well-known mod IDs - must match ModListModel constants
constexpr const char* kProfileOverwriteId = "__overwrite__";

struct ProfileMod {
    std::string mod_id;
    bool enabled = true;
};

class ProfileModel {
public:
    ProfileModel();

    void add_mod(const std::string& mod_id, bool enabled = true);
    void remove_mod(const std::string& mod_id);
    void set_enabled(const std::string& mod_id, bool enabled);
    void move_mod(const std::string& mod_id, uint32_t new_position);

    [[nodiscard]] const std::vector<ProfileMod>& mods() const { return mods_; }

    [[nodiscard]] uint32_t priority_of(const std::string& mod_id) const;

    [[nodiscard]] std::vector<std::string> enabled_in_order() const;

    // Overwrite is always pinned at the end, always enabled.
    void ensure_overwrite_pinned();
    [[nodiscard]] bool is_overwrite(const std::string& mod_id) const;
    [[nodiscard]] int overwrite_index() const;

private:
    std::vector<ProfileMod> mods_;
};

// Backward-compat alias — remove once all call sites use ProfileModel.
using Profile = ProfileModel;

}  // namespace engine
