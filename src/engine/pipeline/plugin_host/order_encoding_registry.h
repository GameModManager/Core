#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

// v2 ABI order-encoding callback signature (gmm_abi_v2.h) - pure C, Qt-free.
#include "gmm_abi_v2.h"

namespace engine {

// v2 ABI order-encoding callback storage (register_order_encoding). A game
// plugin registers a GmmOrderEncodingFnV2 that writes the game's load-order
// file (plugins.txt, metadata.xml, ...). The pipeline retrieves the provider
// for a game and calls it when writing load order, instead of (or in addition
// to) the built-in OrderEncodingHook.
struct OrderEncodingProvider {
    GmmOrderEncodingFnV2 fn = nullptr;
    void* user_data = nullptr;
    std::string game_id;
    std::string plugin_path;
};

class OrderEncodingRegistry {
public:
    static OrderEncodingRegistry& instance();

    // game_id: the game this provider serves (the registering plugin's game).
    void register_provider(const std::string& game_id,
                           GmmOrderEncodingFnV2 fn,
                           void* user_data,
                           const std::string& plugin_path);

    // Get the provider for a game (or nullptr if none registered).
    [[nodiscard]] const OrderEncodingProvider* get_provider(
        const std::string& game_id) const;

    // Convenience: encode the ordered mod ids to output_path for game_id.
    // Returns false when no provider is registered or the callback fails.
    [[nodiscard]] bool encode(const std::string& game_id,
                              const std::vector<std::string>& ordered_mod_ids,
                              const std::filesystem::path& output_path) const;

    // Drop every provider registered by a specific plugin (dlclose path).
    void clear_plugin(const std::string& plugin_path);

    // Drop all providers (full reload path).
    void clear();

private:
    OrderEncodingRegistry() = default;

    std::vector<OrderEncodingProvider> providers_;
};

}  // namespace engine
