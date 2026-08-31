#pragma once

#include "engine/mod/meta/category_set.h"

#include <string>
#include <unordered_map>

namespace engine {

// Registry of named core category sets. A singleton, matching the
// Category::Factory::instance() pattern. Built-in sets ("Default", "Bethesda",
// "Isaac") are registered in the constructor; plugins or future
// user/third-party code can add more via register_set().
//
// Qt-free: this lives in gmm_engine and must not depend on Qt.
class CategorySetRegistry {
public:
  static CategorySetRegistry &instance();

  // Register (or replace) a named set.
  void register_set(CategorySetDefinition set);

  // Look up a set by name. Returns nullptr when unknown - callers handle
  // gracefully (e.g. fall back to "Default").
  [[nodiscard]] const CategorySetDefinition *
  find(const std::string &set_name) const;

  // True when a set with this name is registered.
  [[nodiscard]] bool has(const std::string &set_name) const;

private:
  CategorySetRegistry();
  void register_builtin_sets();

  std::unordered_map<std::string, CategorySetDefinition> sets_;
};

} // namespace engine
