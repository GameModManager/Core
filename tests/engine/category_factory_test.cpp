// Engine test for the global Category::Factory registry.
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
  using engine::Category::Factory;

  // The factory is a singleton; each CTest test runs in its own process, but
  // reset the ids this test touches so it also passes when the binary is run
  // directly (all TEST_CASEs in one process).
  Factory &f = Factory::instance();
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

TEST_CASE("category_factory_apply_core_set", "[engine]") {
  using engine::Category::Factory;

  Factory &f = Factory::instance();

  // All ids the core sets may introduce, so each scenario starts clean even
  // when every TEST_CASE runs in one process.
  std::vector<int> bethesda_ids;
  for (int i = 1; i <= 58; ++i)
    bethesda_ids.push_back(i);
  std::vector<int> isaac_ids;
  for (int i = 1000; i <= 1021; ++i)
    isaac_ids.push_back(i);

  auto clear_all = [&]() {
    for (int id : bethesda_ids)
      f.removeCategory(id);
    for (int id : isaac_ids)
      f.removeCategory(id);
    f.removeCategory(5000);
  };

  // --- Unknown set: returns false and adds nothing. ---
  clear_all();
  require(!f.applyCoreSet("NoSuchSet"),
          "applyCoreSet returns false for unknown");
  require(f.categories().empty(), "unknown set adds no categories");

  // --- "Isaac" set: 22 categories applied, returns true. ---
  clear_all();
  require(f.applyCoreSet("Isaac"), "applyCoreSet(Isaac) returns true");
  require(f.categories().size() == 22, "Isaac set adds 22 categories");
  const auto *items = f.categoryById(1000);
  require(items && items->name == "Items" && items->parent_id == 0,
          "Isaac Items (1000) applied as root");
  const auto *active = f.categoryById(1001);
  require(active && active->name == "Active Items" && active->parent_id == 1000,
          "Isaac Active Items (1001) applied under Items");
  require(items && items->hasChildren, "parent hasChildren recomputed");

  // --- Additive: applying again (or a second set) does not remove existing.
  // ---
  f.addCategory(5000, "User Category", 0);
  require(f.applyCoreSet("Isaac"), "re-applying Isaac is a no-op success");
  require(f.categoryExists(5000), "user category preserved across re-apply");
  require(f.categories().size() == 23, "no duplicate Isaac ids added");

  // --- "Bethesda" set: 57 entries (MO2 repeats id 39 for Voice/Tattoos), so 56
  // unique categories survive the by-id dedupe. Separate id space from Isaac.
  // ---
  clear_all();
  require(f.applyCoreSet("Bethesda"), "applyCoreSet(Bethesda) returns true");
  require(f.categories().size() == 56,
          "Bethesda set adds 56 unique categories");
  const auto *anim = f.categoryById(1);
  require(anim && anim->name == "Animations" && anim->parent_id == 0,
          "Bethesda Animations (1) applied as root");
  const auto *combat = f.categoryById(27);
  require(combat && combat->name == "Combat" && combat->parent_id == 9,
          "Bethesda Combat (27) applied under Gameplay (9)");

  // --- "Default" set: registered but empty, returns true and adds nothing. ---
  clear_all();
  require(f.applyCoreSet("Default"), "applyCoreSet(Default) returns true");
  require(f.categories().empty(), "Default set adds no categories");

  // Final cleanup.
  clear_all();
}

TEST_CASE("category_factory_clear_drops_all_entries", "[engine]") {
  using engine::Category::Factory;

  Factory &f = Factory::instance();

  // Seed with two distinct sets so the test starts from a known non-empty
  // state regardless of test order (CTest runs each binary in its own
  // process, but the tests above already exercise the applyCoreSet paths).
  std::vector<int> bethesda_ids;
  for (int i = 1; i <= 58; ++i)
    bethesda_ids.push_back(i);
  std::vector<int> isaac_ids;
  for (int i = 1000; i <= 1021; ++i)
    isaac_ids.push_back(i);

  auto reset = [&]() {
    for (int id : bethesda_ids)
      f.removeCategory(id);
    for (int id : isaac_ids)
      f.removeCategory(id);
  };

  // --- Two sets layered on top of each other. ---
  reset();
  require(f.applyCoreSet("Bethesda"), "Bethesda applied");
  require(f.applyCoreSet("Isaac"), "Isaac applied on top of Bethesda");
  require(f.categoryExists(1), "Bethesda entry present (Animations)");
  require(f.categoryExists(1000), "Isaac entry present (Items)");

  // --- clear() drops every entry. ---
  f.clear();
  require(f.categories().empty(), "clear empties the factory");
  require(!f.categoryExists(1), "Bethesda entry gone after clear");
  require(!f.categoryExists(1000), "Isaac entry gone after clear");

  // --- applyCoreSet after clear produces exactly the set (no leakage). ---
  require(f.applyCoreSet("Bethesda"),
          "Bethesda applied again after clear");
  require(!f.categoryExists(1000),
          "no Isaac categories leak after clear+applyCoreSet(Bethesda)");
  require(f.categoryById(1) && f.categoryById(1)->name == "Animations",
          "Bethesda Animations present");

  // --- reset hasChildren is consistent after clear. ---
  f.clear();
  require(!f.categoryById(1), "no entries means no hasChildren flags");

  reset();
}