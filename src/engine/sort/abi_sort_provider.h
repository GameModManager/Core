#pragma once

#include "engine/sort/sort_provider.h"

#include <functional>
#include <string>
#include <vector>

namespace engine {

// C ABI sort function type
typedef const char* const* (*SortFn)(const char* const* mod_folders, size_t count, void* user_data);

// Wrapper that converts C ABI sort function to SortProvider
class AbiSortProvider : public SortProvider {
public:
    AbiSortProvider(const char* game_id, SortFn sort_fn, void* user_data);

    ModSortResult sort(const std::vector<SortModInfo>& mods) const override;
    const char* name() const override { return "ABI Sort Provider"; }

private:
    std::string game_id_;
    SortFn sort_fn_;
    void* user_data_;
};

}  // namespace engine
