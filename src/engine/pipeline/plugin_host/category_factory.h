#pragma once

#include <filesystem>
#include <map>
#include <string>

namespace engine {

// Global category registry — one shared set across all games.
// Plugins register categories via the ABI register_categories callback;
// this factory merges them by ID (no duplicates). Parent ID 0 = root.
// Persisted to categories.dat as pipe-delimited rows: ID|Name|ParentID.
class CategoryFactory {
public:
  struct Category {
    int id = 0;
    std::string name;
    int parent_id = 0;
    bool hasChildren = false; // computed by rebuildTree()
  };

  static CategoryFactory &instance() {
    static CategoryFactory s;
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

  // --- Lookups ---
  [[nodiscard]] bool categoryExists(int id) const;
  [[nodiscard]] const Category *categoryById(int id) const;
  [[nodiscard]] const std::map<int, Category> &categories() const {
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
  CategoryFactory() = default;
  void updateHasChildren();

  std::map<int, Category> categories_;
};

} // namespace engine