#pragma once

#include "engine/sort/sort_provider.h"

#include <memory>
#include <string>
#include <vector>

namespace engine {

class SortRegistry {
public:
  static SortRegistry &instance();

  // Register a sort provider for a game
  void register_provider(const std::string &game_id,
                         std::unique_ptr<Sorter::Interface> provider);

  // Get the sort provider for a game (or nullptr if none)
  [[nodiscard]] Sorter::Interface *
  get_provider(const std::string &game_id) const;

  // Drop every registered provider (process shutdown / full reload).
  void clear();

private:
  SortRegistry() = default;
  std::vector<std::pair<std::string, std::unique_ptr<Sorter::Interface>>>
      providers_;
};

} // namespace engine
