#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace engine {

// Per-instance category database, MO2-compatible. Two files:
//   <instance>/categories.dat    - one line per internal category:
//                                  id|name|parentId (or
//                                  id|name|nexusIds|parentId)
//   <instance>/nexuscatmap.dat   - Nexus category -> internal category mapping:
//                                  categoryId|name|nexusId
// Layout and serialization mirror MO2's Category::Factory (categories.cpp).
class Categories {
public:
  // Nexus category (as reported by the Nexus API / catmap).
  struct NexusCat {
    std::string name;
    int nexus_id = 0;    // Nexus category id (maps to this internal one)
    int category_id = 0; // internal category this Nexus id maps to
  };

  // Internal category. `nexus_ids` are Nexus categories mapped to it.
  struct Category {
    int id = 0;
    std::string name;
    std::vector<int> nexus_ids;
    int parent_id = 0;
    bool has_children = false;
  };

  Categories() { seed_default(); }

  // --- File I/O ---
  // Loads both files from the instance root; seeds the MO2 default list when
  // categories.dat is missing. Never throws.
  static Categories load(const std::filesystem::path &instance_root);
  void save(const std::filesystem::path &instance_root) const;

  // --- MO2 default list ---
  void seed_default();

  // --- Queries ---
  const std::vector<Category> &categories() const { return categories_; }
  bool contains(int id) const;
  const Category *find(int id) const; // last entry wins (MO2 ID-map)
  std::vector<const Category *> children_of(int parent_id) const;

  // Nexus mapping lookups (from nexuscatmap.dat).
  bool has_nexus(int nexus_id) const;
  const NexusCat *nexus(int nexus_id) const;
  const Category *category_for_nexus(int nexus_id) const;

  // --- Mutation ---
  void add_category(int id, const std::string &name, int parent_id = 0);
  void add_nexus_mapping(int internal_id, const std::string &name,
                         int nexus_id);
  void remove_category(int id);
  void set_parent(int id, int parent_id);

  // Recomputes the has_children flags and drops dangling parents.
  void rebuild_tree();

private:
  void update_has_children();
  void load_from_lines(const std::vector<std::string> &lines);
  void load_nexus_map(const std::vector<std::string> &lines);

  std::vector<Category> categories_;
  // nexus_id -> NexusCat, ordered for deterministic serialization.
  std::map<int, NexusCat> nexus_map_;
};

} // namespace engine
