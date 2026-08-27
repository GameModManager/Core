#pragma once

#include "engine/deploy/order_hook.h"

#include <string>
#include <vector>

namespace engine {

// Adapter that wraps a v2 plugin's GmmOrderEncodingFnV2 callback into the
// engine's OrderEncodingHook interface. The pipeline invokes write_order()
// when persisting load-order files (plugins.txt, metadata.xml, etc.); this
// adapter forwards to the plugin's callback, passing the ordered mod IDs and
// the output path.
class AbiOrderEncodingHook : public OrderEncodingHook {
public:
    using Fn = int (*)(const char* const*, size_t, const char*, void*);

    AbiOrderEncodingHook(Fn fn, void* user_data)
        : fn_(fn), user_data_(user_data) {}

    bool write_order(const std::vector<std::string>& ordered_mod_ids,
                     const std::filesystem::path& output_path) override {
        if (!fn_) return false;

        // Build a C-style array of const char* for the ABI call.
        std::vector<const char*> c_mods;
        c_mods.reserve(ordered_mod_ids.size());
        for (const auto& id : ordered_mod_ids) {
            c_mods.push_back(id.c_str());
        }

        const int result = fn_(c_mods.data(), c_mods.size(),
                               output_path.string().c_str(), user_data_);
        return result != 0;
    }

private:
    Fn fn_;
    void* user_data_;
};

}  // namespace engine
