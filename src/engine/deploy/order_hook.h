#pragma once

#include "engine/model/profile.h"

#include <filesystem>
#include <string>
#include <vector>

namespace engine {

class OrderEncodingHook {
public:
    virtual ~OrderEncodingHook() = default;
    virtual bool write_order(const std::vector<std::string>& ordered_mod_ids,
                             const std::filesystem::path& output_path) = 0;
};

class SkyrimPluginsTxtHook : public OrderEncodingHook {
public:
    bool write_order(const std::vector<std::string>& ordered_mod_ids,
                     const std::filesystem::path& output_path) override;
};

}  // namespace engine
