#include "engine/sort/sort_registry.h"

namespace engine {

SortRegistry &SortRegistry::instance() {
  static SortRegistry registry;
  return registry;
}

void SortRegistry::register_provider(
    const std::string &game_id, std::unique_ptr<Sorter::Interface> provider) {
  // Remove existing provider for this game
  for (auto it = providers_.begin(); it != providers_.end(); ++it) {
    if (it->first == game_id) {
      providers_.erase(it);
      break;
    }
  }
  providers_.emplace_back(game_id, std::move(provider));
}

Sorter::Interface *
SortRegistry::get_provider(const std::string &game_id) const {
  for (const auto &[id, provider] : providers_) {
    if (id == game_id) {
      return provider.get();
    }
  }
  return nullptr;
}

void SortRegistry::clear() { providers_.clear(); }

} // namespace engine
