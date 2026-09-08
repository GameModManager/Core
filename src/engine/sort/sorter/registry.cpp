#include "engine/sort/sorter/registry.h"

namespace engine {
namespace Sorter {

Registry &Registry::instance() {
  static Registry registry;
  return registry;
}

void Registry::register_provider(
    const std::string &game_id, std::unique_ptr<Interface> provider) {
  // Remove existing provider for this game
  for (auto it = providers_.begin(); it != providers_.end(); ++it) {
    if (it->first == game_id) {
      providers_.erase(it);
      break;
    }
  }
  providers_.emplace_back(game_id, std::move(provider));
}

Interface *
Registry::get_provider(const std::string &game_id) const {
  for (const auto &[id, provider] : providers_) {
    if (id == game_id) {
      return provider.get();
    }
  }
  return nullptr;
}

void Registry::clear() { providers_.clear(); }

} // namespace Sorter
} // namespace engine
