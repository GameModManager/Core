#include "engine/pipeline/plugin_host/order_encoding_registry.h"

#include "engine/core/log/logger.h"

#include <algorithm>

namespace engine {

OrderEncodingRegistry& OrderEncodingRegistry::instance() {
    static OrderEncodingRegistry inst;
    return inst;
}

void OrderEncodingRegistry::register_provider(const std::string& game_id,
                                              GmmOrderEncodingFnV2 fn,
                                              void* user_data,
                                              const std::string& plugin_path) {
    if (!fn) return;
    OrderEncodingProvider p;
    p.game_id = game_id;
    p.fn = fn;
    p.user_data = user_data;
    p.plugin_path = plugin_path;
    providers_.push_back(std::move(p));
    Logger::instance().debug("OrderEncodingRegistry: registered provider for game=" +
        game_id + " (plugin=" + plugin_path + ")");
}

const OrderEncodingProvider* OrderEncodingRegistry::get_provider(
    const std::string& game_id) const {
    auto it = std::find_if(providers_.begin(), providers_.end(),
        [&game_id](const OrderEncodingProvider& p) {
            return p.game_id == game_id;
        });
    return it == providers_.end() ? nullptr : &*it;
}

bool OrderEncodingRegistry::encode(const std::string& game_id,
                                   const std::vector<std::string>& ordered_mod_ids,
                                   const std::filesystem::path& output_path) const {
    const OrderEncodingProvider* p = get_provider(game_id);
    if (!p || !p->fn) return false;

    std::vector<const char*> ids;
    ids.reserve(ordered_mod_ids.size());
    for (const auto& s : ordered_mod_ids) ids.push_back(s.c_str());

    const int ok = p->fn(ids.empty() ? nullptr : ids.data(), ids.size(),
                         output_path.string().c_str(), p->user_data);
    return ok != 0;
}

void OrderEncodingRegistry::clear_plugin(const std::string& plugin_path) {
    providers_.erase(
        std::remove_if(providers_.begin(), providers_.end(),
            [&plugin_path](const OrderEncodingProvider& p) {
                return p.plugin_path == plugin_path;
            }),
        providers_.end());
}

void OrderEncodingRegistry::clear() {
    providers_.clear();
}

}  // namespace engine
