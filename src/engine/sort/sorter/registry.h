#pragma once

#include "engine/sort/sorter/interface.h"

#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace Sorter {

class Registry {
public:
  static Registry &instance();

  // Register a sort provider for a game
  void register_provider(const std::string &game_id,
                         std::unique_ptr<Interface> provider);

  // Get the sort provider for a game (or nullptr if none)
  [[nodiscard]] Interface *
  get_provider(const std::string &game_id) const;

  // Drop every registered provider (process shutdown / full reload).
  void clear();

private:
  Registry() = default;
  std::vector<std::pair<std::string, std::unique_ptr<Interface>>>
      providers_;
};

} // namespace Sorter
} // namespace engine
