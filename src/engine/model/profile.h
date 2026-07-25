#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace engine {

struct ProfileMod {
    std::string mod_id;
    bool enabled = true;
};

class Profile {
public:
    void add_mod(const std::string& mod_id, bool enabled = true);
    void remove_mod(const std::string& mod_id);
    void set_enabled(const std::string& mod_id, bool enabled);
    void move_mod(const std::string& mod_id, uint32_t new_position);

    [[nodiscard]] const std::vector<ProfileMod>& mods() const { return mods_; }

    [[nodiscard]] uint32_t priority_of(const std::string& mod_id) const;

    [[nodiscard]] std::vector<std::string> enabled_in_order() const;

private:
    std::vector<ProfileMod> mods_;
};

}  // namespace engine
