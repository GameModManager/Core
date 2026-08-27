#pragma once

#include <filesystem>
#include <string>
#include <vector>

// v2 ABI deploy-strategy callback signatures (gmm_abi_v2.h) - pure C, Qt-free.
#include "gmm_abi_v2.h"

namespace engine {

// v2 ABI deploy-strategy callback storage (register_deploy_strategy). A game
// plugin registers GmmDeployFnV2 / GmmRemoveFnV2 that place/remove a single
// mod file into/out of the game data directory. The pipeline retrieves the
// provider for a game and calls it instead of the built-in DeploymentStrategy.
struct DeployStrategyProvider {
    GmmDeployFnV2 deploy_fn = nullptr;
    GmmRemoveFnV2 remove_fn = nullptr;
    void* user_data = nullptr;
    std::string game_id;
    std::string plugin_path;
};

class DeployStrategyRegistry {
public:
    static DeployStrategyRegistry& instance();

    // game_id: the game this provider serves (the registering plugin's game).
    void register_provider(const std::string& game_id,
                           GmmDeployFnV2 deploy_fn,
                           GmmRemoveFnV2 remove_fn,
                           void* user_data,
                           const std::string& plugin_path);

    // Get the provider for a game (or nullptr if none registered).
    [[nodiscard]] const DeployStrategyProvider* get_provider(
        const std::string& game_id) const;

    // Convenience: deploy source -> target for game_id. Returns false when no
    // provider is registered or the callback fails.
    [[nodiscard]] bool deploy(const std::string& game_id,
                              const std::filesystem::path& source,
                              const std::filesystem::path& target) const;

    // Convenience: remove target for game_id. Returns false when no provider is
    // registered or the callback fails.
    [[nodiscard]] bool remove(const std::string& game_id,
                              const std::filesystem::path& target) const;

    // Drop every provider registered by a specific plugin (dlclose path).
    void clear_plugin(const std::string& plugin_path);

    // Drop all providers (full reload path).
    void clear();

private:
    DeployStrategyRegistry() = default;

    std::vector<DeployStrategyProvider> providers_;
};

}  // namespace engine
