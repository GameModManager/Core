#pragma once

#include <string>
#include <vector>

namespace engine {

struct SortModInfo {
  std::string folder_name;
  std::string display_name;
  int64_t workshop_id = 0;
  std::vector<std::string> tags; // from metadata.xml
};

struct ModSortResult {
  std::vector<std::string>
      sorted_folders; // folder names in load order (top = first)
  struct TagInfo {
    std::string folder_name;
    std::string type; // "deprecated", "note", "warning", etc.
    std::string message;
  };
  std::vector<TagInfo> tags; // tags to apply to mods
};

namespace Sorter {

class Interface {
public:
  virtual ~Interface() = default;

  // Sort mods and evaluate tags
  virtual ModSortResult sort(const std::vector<SortModInfo> &mods) const = 0;

  // Provider name for logging
  virtual const char *name() const = 0;
};

} // namespace Sorter

// Backward-compat alias — old code refers to SortProvider directly
using SortProvider = Sorter::Interface;

} // namespace engine
