#pragma once

#include "engine/sort/sorter/interface.h"

#include <functional>
#include <string>
#include <vector>

namespace engine {

// C ABI sort function type
typedef const char *const *(*SortFn)(const char *const *mod_folders,
                                     size_t count, void *user_data);

namespace Sorter {

// Wrapper that converts C ABI sort function to Sorter::Interface
class Abi : public Interface {
public:
  Abi(const char *game_id, SortFn sort_fn, void *user_data);

  Result sort(const std::vector<ModInfo> &mods) const override;
  const char *name() const override { return "ABI Sort Provider"; }

private:
  std::string game_id_;
  SortFn sort_fn_;
  void *user_data_;
};

} // namespace Sorter
} // namespace engine
