// Nexus category import (engine/source/nexus/categories).
//
// The branch that matters is the response shape: Nexus describes a category's
// parent as EITHER the owning category's id OR the literal false for a root
// category, so a parser that only ever reads a number silently turns every
// root category into a child of category 0 - or drops the whole body. Both
// were live ways to end up with a wrong tree for a real game.
//
// The ids here are deliberately per-game, because they are: the DTO says
// "Mod categories. Unique per game." (references/NexusMods.App/src/
// NexusMods.Abstractions.NexusWebApi/DTOs/Category.cs). Nothing in this file
// treats a category id as global.
#include "engine/source/nexus/categories.h"

#include "engine/plugin_host/category_factory.h"

#include <catch2/catch_test_macros.hpp>

namespace {
void check(bool cond, const char *what) {
  INFO(what);
  REQUIRE(cond);
}
}  // namespace

using engine::Source::Nexus::CategoryList;
using engine::Source::Nexus::NexusCategory;
using engine::Source::Nexus::parse_categories;

TEST_CASE("nexus category import reads the per-game category tree",
          "[engine][categories]") {
  // A trimmed slice of a real game's list: a root, a child of it, and a
  // second root. Roots carry "parent_category": false.
  const CategoryList list = parse_categories(R"({
    "data": {
      "categories": [
        {"category_id": 1, "name": "Animations", "parent_category": false},
        {"category_id": 52, "name": "Poses", "parent_category": 1},
        {"category_id": 2, "name": "Armour", "parent_category": false}
      ]
    }
  })");

  check(list.ok, "a well-formed body parses");
  check(list.categories.size() == 3, "all three categories come through");

  check(list.categories[0].id == 1 && list.categories[0].name == "Animations",
        "root category keeps its id and name");
  check(list.categories[0].parent_id == 0,
        "parent_category false means root, not parent 0's child");

  check(list.categories[1].id == 52 && list.categories[1].parent_id == 1,
        "a child keeps the parent id it was given");

  // The whole reason the parent field is fussy: the same field is a number
  // for a child and a boolean for a root.
  check(list.categories[2].parent_id == 0,
        "a second root is a root too, not a child of the first");

  // --- Bad input: ok=false, never a half-built list. ---
  check(!parse_categories("").ok, "an empty body is not a category list");
  check(!parse_categories("not json at all").ok, "garbage is not a category list");
  check(!parse_categories(R"({"data":{}})").ok, "a body with no categories key fails");
  check(!parse_categories(R"({"categories":[]})").ok,
        "an unwrapped body is rejected: Nexus wraps in data");
  check(parse_categories(R"({"data":{"categories":[]}})").categories.empty(),
        "an empty category array yields no categories");

  // A row with no usable id is dropped, but the rest of the body still lands.
  const CategoryList partial = parse_categories(R"({
    "data": {"categories": [
      {"name": "No id at all", "parent_category": false},
      {"category_id": 7, "name": "Keep me", "parent_category": false}
    ]}
  })");
  check(partial.ok && partial.categories.size() == 1,
        "a row with no category_id is dropped, the usable one survives");
  check(partial.categories[0].id == 7, "the surviving row is the usable one");
}

TEST_CASE("a parsed nexus list reaches the active category set",
          "[engine][categories]") {
  // The parser on its own proves nothing about the product: a fetched list is
  // only useful if it lands where the UI reads categories from. This walks the
  // real path the Categories dialog takes - parse, then import into
  // Category::Factory - with no shim between them.
  using engine::Category::Factory;

  Factory &f = Factory::instance();
  f.activate_game("nexusimportgame");

  const CategoryList list = parse_categories(R"({
    "data": {"categories": [
      {"category_id": 1, "name": "Animations", "parent_category": false},
      {"category_id": 52, "name": "Poses", "parent_category": 1}
    ]}
  })");
  check(list.ok, "the list to import parsed");

  std::vector<Factory::Entry> entries;
  for (const auto &cat : list.categories) {
    Factory::Entry entry;
    entry.id        = cat.id;
    entry.name      = cat.name;
    entry.parent_id = cat.parent_id;
    entries.push_back(std::move(entry));
  }

  const int added = f.import_external(entries, Factory::ImportMode::Merge);
  check(added == 2, "both categories are added to the active set");
  check(f.categoryExists(1) && f.categoryById(1)->name == "Animations",
        "the imported root is readable from the factory the UI reads");
  check(f.categoryById(52)->parent_id == 1,
        "the imported parent link survives the import");

  // Merge is idempotent: pressing import twice must not duplicate a row. A user
  // who clicks it twice should not end up with a doubled list.
  check(f.import_external(entries, Factory::ImportMode::Merge) == 0,
        "re-importing the same list adds nothing");
  check(f.categories().size() == 2, "and does not duplicate anything");

  // Replace drops what was active first (MO2's "Overwrite" strategy).
  const int replaced = f.import_external(entries, Factory::ImportMode::Replace);
  check(replaced == 2 && f.categories().size() == 2,
        "Replace reinstalls the same list without accumulating");

  // A parent that is not in the imported list must not import as a dangling
  // link, because every tree that walks the map renders it.
  const int orphan = f.import_external({Factory::Entry{900, "Orphan", 999}},
                                       Factory::ImportMode::Merge);
  check(orphan == 1, "the row itself still imports");
  check(f.categoryById(900)->parent_id == 0,
        "a parent that does not exist degrades to root, not to a dangling id");
}