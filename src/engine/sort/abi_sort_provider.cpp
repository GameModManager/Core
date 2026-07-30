#include "engine/sort/abi_sort_provider.h"

#include <cstring>

namespace engine {

AbiSortProvider::AbiSortProvider(const char* game_id, SortFn sort_fn, void* user_data)
    : game_id_(game_id ? game_id : "")
    , sort_fn_(sort_fn)
    , user_data_(user_data) {}

ModSortResult AbiSortProvider::sort(const std::vector<SortModInfo>& mods) const {
    ModSortResult result;

    if (!sort_fn_) return result;

    // Build input array of mod folder names
    std::vector<const char*> mod_folders;
    for (const auto& mod : mods) {
        mod_folders.push_back(mod.folder_name.c_str());
    }

    // Call the sort function
    const char* const* sorted = sort_fn_(mod_folders.data(), mod_folders.size(), user_data_);

    // Copy sorted results
    if (sorted) {
        for (size_t i = 0; sorted[i] != nullptr; ++i) {
            result.sorted_folders.push_back(sorted[i]);
        }
    }

    return result;
}

}  // namespace engine
