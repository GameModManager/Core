// Engine test for the global CategoryFactory registry.
//
// Covers: merge from parallel arrays (duplicates skipped by ID), load/save of
// the pipe-delimited categories.dat (ID|Name|ParentID), parent-child hierarchy
// (hasChildren flag), and add/remove/update mutations.
#include "engine/pipeline/plugin_host/category_factory.h"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {
void require(bool cond, const char *msg) {
  INFO(msg);
  REQUIRE(cond);
}
} // namespace

static void write_file(const fs::path &path, const std::string &content) {
  std::ofstream out(path);
  out << content;
}

TEST_CASE("category_factory", "[engine]") {
  using engine::CategoryFactory;

  // The factory is a singleton; each CTest test runs in its own process, but
  // reset the ids this test touches so it also passes when the binary is run
  // directly (all TEST_CASEs in one process).
  CategoryFactory &f = CategoryFactory::instance();
  f.removeCategory(1);
  f.removeCategory(2);
  f.removeCategory(3);
  f.removeCategory(10);
  f.removeCategory(11);

  // --- Merge from parallel arrays (ABI register_categories). ---
  int ids[] = {1, 2, 1, 3};
  const char *names[] = {"Animations", "Armour", "Duplicate", "Poses"};
  int parents[] = {0, 0, 0, 1};
  f.merge(ids, names, parents, 4);
  require(f.categoryExists(1), "id 1 merged");
  require(f.categoryExists(2), "id 2 merged");
  require(f.categoryExists(3), "id 3 merged");
  const auto *anim = f.categoryById(1);
  require(anim && anim->name == "Animations",
          "duplicate id 1 skipped (first name wins)");
  require(anim && anim->hasChildren, "id 1 has child 3");
  const auto *poses = f.categoryById(3);
  require(poses && poses->parent_id == 1, "Poses is child of Animations");

  // --- addCategory / updateCategory / removeCategory. ---
  f.addCategory(10, "My Mods", 0);
  f.addCategory(11, "Child Mods", 10);
  require(f.categoryById(10)->hasChildren, "hasChildren true after add");
  f.addCategory(10, "Duplicate Add", 0);
  require(f.categoryById(10)->name == "My Mods",
          "addCategory skips an existing id");
  f.updateCategory(11, "Renamed Child", 0);
  require(f.categoryById(11)->name == "Renamed Child" &&
              f.categoryById(11)->parent_id == 0,
          "updateCategory changes name and parent");
  require(!f.categoryById(10)->hasChildren,
          "hasChildren recomputed after child re-parented");
  f.removeCategory(10);
  require(!f.categoryExists(10), "removeCategory removes the category");
  require(f.categoryById(11)->parent_id == 0,
          "removeCategory re-parents children to root");

  // --- Load/save round-trip. ---
  const fs::path root = "/tmp/gmm_category_factory_test";
  fs::remove_all(root);
  fs::create_directories(root);
  write_file(root / "categories.dat", "1|Animations|0\n"
                                      "3|Poses|1\n"
                                      "2|Armour|0\n");
  f.load(root / "categories.dat");
  require(f.categoryExists(1) && f.categoryExists(2) && f.categoryExists(3),
          "load reads categories.dat");
  require(f.categoryById(1)->hasChildren, "hasChildren computed on load");
  f.save(root / "categories.dat");
  std::ifstream in(root / "categories.dat");
  std::string saved((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
  require(saved == "1|Animations|0\n2|Armour|0\n3|Poses|1\n",
          "categories.dat round-trips (map writes in ID order)");

  // --- Load replaces the current set; missing file keeps it. ---
  f.addCategory(99, "Transient", 0);
  f.load(root / "categories.dat");
  require(!f.categoryExists(99), "load replaces the current set");
  f.load(root / "does_not_exist.dat");
  require(f.categoryExists(1), "missing file leaves the registry unchanged");
}