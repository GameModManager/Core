#pragma once

#include <filesystem>
#include <map>
#include <string>

namespace engine {

namespace Category {

// Global category registry — one shared set across all games.
// Plugins register categories via the ABI register_categories callback;
// this factory merges them by ID (no duplicates). Parent ID 0 = root.
// Persisted to categories.dat as pipe-delimited rows: ID|Name|ParentID.
class Factory {
public:
  struct Entry {
    int id = 0;
    std::string name;
    int parent_id = 0;
    bool hasChildren = false; // computed by rebuildTree()
  };

  static Factory &instance() {
    static Factory s;
    return s;
  }

  // --- File I/O (categories.dat: ID|Name|ParentID per line) ---
  // Replaces the current set. A missing/unreadable file leaves the registry
  // unchanged. Never throws.
  void load(const std::filesystem::path &path);
  void save(const std::filesystem::path &path) const;

  // Merge categories from parallel arrays (ABI register_categories).
  // Duplicates by ID are skipped. parent_id 0 = root category.
  void merge(const int *ids, const char *const *names, const int *parent_ids,
             size_t count);

  // Populates the factory from a named core set (see CategorySetRegistry).
  // Existing categories are NOT cleared — the set adds missing categories only
  // (additive). Returns true when the set was found and applied.
  bool applyCoreSet(const std::string &set_name);

  // --- Lookups ---
  [[nodiscard]] bool categoryExists(int id) const;
  [[nodiscard]] const Entry *categoryById(int id) const;
  [[nodiscard]] const std::map<int, Entry> &categories() const {
    return categories_;
  }

  // --- Mutation ---
  // Adds a new category; no-op when the id already exists (or id == 0).
  void addCategory(int id, const std::string &name, int parent_id = 0);
  // Removes a category and re-parents its direct children to root (0).
  void removeCategory(int id);
  // Updates the name and parent of an existing category; no-op when missing.
  void updateCategory(int id, const std::string &name, int parent_id);

  // Recomputes the hasChildren flags after any structural change.
  void rebuildTree();

private:
  Factory() = default;
  void updateHasChildren();

  std::map<int, Entry> categories_;
};

} // namespace Category

// Backward-compatible alias for existing consumers
using CategoryFactory = Category::Factory;

} // namespace engine
