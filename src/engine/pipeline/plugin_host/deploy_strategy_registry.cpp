#include "engine/pipeline/plugin_host/deploy_strategy_registry.h"

#include "engine/core/log/logger.h"

#include <algorithm>

namespace engine {

DeployStrategyRegistry& DeployStrategyRegistry::instance() {
    static DeployStrategyRegistry inst;
    return inst;
}

void DeployStrategyRegistry::register_provider(const std::string& game_id,
                                               GmmDeployFnV2 deploy_fn,
                                               GmmRemoveFnV2 remove_fn,
                                               void* user_data,
                                               const std::string& plugin_path) {
    DeployStrategyProvider p;
    p.game_id = game_id;
    p.deploy_fn = deploy_fn;
    p.remove_fn = remove_fn;
    p.user_data = user_data;
    p.plugin_path = plugin_path;
    providers_.push_back(std::move(p));
    Logger::instance().debug("DeployStrategyRegistry: registered provider for game=" +
        game_id + " (plugin=" + plugin_path + ")");
}

const DeployStrategyProvider* DeployStrategyRegistry::get_provider(
    const std::string& game_id) const {
    auto it = std::find_if(providers_.begin(), providers_.end(),
        [&game_id](const DeployStrategyProvider& p) {
            return p.game_id == game_id;
        });
    return it == providers_.end() ? nullptr : &*it;
}

bool DeployStrategyRegistry::deploy(const std::string& game_id,
                                    const std::filesystem::path& source,
                                    const std::filesystem::path& target) const {
    const DeployStrategyProvider* p = get_provider(game_id);
    if (!p || !p->deploy_fn) return false;
    const int ok = p->deploy_fn(source.string().c_str(),
                                target.string().c_str(), p->user_data);
    return ok != 0;
}

bool DeployStrategyRegistry::remove(const std::string& game_id,
                                    const std::filesystem::path& target) const {
    const DeployStrategyProvider* p = get_provider(game_id);
    if (!p || !p->remove_fn) return false;
    const int ok = p->remove_fn(target.string().c_str(), p->user_data);
    return ok != 0;
}

void DeployStrategyRegistry::clear_plugin(const std::string& plugin_path) {
    providers_.erase(
        std::remove_if(providers_.begin(), providers_.end(),
            [&plugin_path](const DeployStrategyProvider& p) {
                return p.plugin_path == plugin_path;
            }),
        providers_.end());
}

void DeployStrategyRegistry::clear() {
    providers_.clear();
}

}  // namespace engine
