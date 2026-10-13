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

#include "engine/mod/meta/categories.h"
#include "engine/plugin_host/category_factory.h"

#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <filesystem>

namespace {
void check(bool cond, const char *what) {
  INFO(what);
  REQUIRE(cond);
}

struct TempDir {
  std::filesystem::path root;
  TempDir() {
    root = std::filesystem::temp_directory_path() /
           ("gmm_nexus_catmap_" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }
};
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

TEST_CASE("an imported category records the nexus mapping, and resolve_nexus_id "
          "reads it back through the file the UI loads",
          "[engine][categories]") {
  // The install-time half of the feature: a mod installed from Nexus carries a
  // per-game Nexus category id, and the local category it belongs in is only
  // knowable through the mapping the import wrote. Before this, nothing in the
  // tree ever wrote a mapping, so resolve_nexus_id() could only ever answer 0.
  //
  // MO2 stores the pair of files the same way and resolves at install time:
  // CategoryFactory::resolveNexusID
  // (references/modorganizer/src/categories.cpp:513-525), fed by
  // installationmanager.cpp:668 from the nexuscatmap.dat its own save writes
  // (categories.cpp:213-226).
  using engine::Category::Factory;

  TempDir tmp;
  const std::filesystem::path dat = tmp.root / "categories.dat";

  // --- The import the Categories dialog runs with MO2's default options:
  // assign on, remap off (categoryimportdialog.ui ships them that way). ---
  {
    Factory &f = Factory::instance();
    f.activate_game("nexusresolgame");
    f.import_external({{1, "Animations", 0}, {52, "Poses", 1}},
                      Factory::ImportMode::Merge,
                      /*assign=*/true, /*remap=*/false);

    check(f.resolve_nexus_id(1) == 1, "an imported Nexus id resolves to its own row");
    check(f.resolve_nexus_id(52) == 52,
          "a child category carries its mapping too, not just the root");
    check(f.resolve_nexus_id(999) == 0, "an id nobody imported is unmapped, i.e. None");

    // A mapping whose category has since been removed must not resolve to a
    // dangling id - MO2 checks its ID map for exactly this and returns 0
    // (categories.cpp:517-521).
    f.removeCategory(52);
    check(f.resolve_nexus_id(52) == 0,
          "a mapping pointing at a removed category resolves to None, not to its id");

    f.save(dat);
    check(std::filesystem::exists(tmp.root / "nexuscatmap.dat"),
          "the mapping is persisted beside categories.dat, as MO2 keeps it");
  }

  // --- A fresh session: the factory is rebuilt from disk, and the OTHER store
  // that reads it - engine::Categories, the one the mod list and the Mod Info
  // Categories tab already load from the instance root - finds the mapping.
  // That consumer is the whole point: an installed mod lands in the right local
  // category only if this resolves. ---
  {
    Factory &f = Factory::instance();
    f.activate_game("nexusresolgame");
    f.load(dat);
    check(f.resolve_nexus_id(1) == 1, "the mapping survives a reload from disk");

    const engine::Categories cats = engine::Categories::load(tmp.root);
    const auto *mapped            = cats.category_for_nexus(1);
    check(mapped != nullptr, "the installed-mod consumer resolves the Nexus id");
    check(mapped && mapped->id == 1 && mapped->name == "Animations",
          "and it lands on the internal category, not on a copy of the Nexus id");
  }

  // --- remap: the option MO2 offers for a category the user already has, under
  // their own local id (categoriesdialog.cpp:330-354). ---
  {
    Factory &f = Factory::instance();
    f.activate_game("nexusremapgame");
    // The user's own row: local id 700, named "Animation Replacers".
    f.addCategory(700, "Animation Replacers", 0);

    // Nexus id 52 is "Animation Replacers" for this game. Matching is by name,
    // so the import must attach 52 to the user's row 700 rather than adding a
    // second "Animation Replacers" carrying the Nexus id.
    const int added =
        f.import_external({{52, "Animation Replacers", 0}}, Factory::ImportMode::Merge,
                          /*assign=*/false, /*remap=*/true);
    check(added == 0, "remap adds no row: the name was already in the list");
    check(!f.categoryExists(52), "and in particular no duplicate under the Nexus id");
    check(f.resolve_nexus_id(52) == 700,
          "remap points the Nexus id at the user's own category of that name");
    check(f.categoryById(700)->name == "Animation Replacers",
          "the user's row is untouched - an import never rewrites a category");
  }

  // --- remap off, assign off: the category the user already has is left
  /// alone and no mapping is written, so an installed mod still has nothing to
  // resolve. This is the case the single-button import used to be in, silently.
  {
    Factory &f = Factory::instance();
    f.activate_game("nexusassignoffgame");
    f.addCategory(800, "Audio", 0);
    const int added = f.import_external({{3, "Audio", 0}}, Factory::ImportMode::Merge,
                                        /*assign=*/false, /*remap=*/false);
    check(added == 0, "with remap off an already-present name is not duplicated");
    check(f.resolve_nexus_id(3) == 0,
          "and with assign off too there is deliberately no mapping to resolve");
  }

  // --- assign off, remap off: a name nobody has yet still imports as a row,
  // it just carries no mapping. That is the difference the two options make. ---
  {
    Factory &f = Factory::instance();
    f.activate_game("nexusassignoffnew");
    f.import_external({{1, "Audio", 0}}, Factory::ImportMode::Merge,
                      /*assign=*/false, /*remap=*/false);
    check(f.categoryExists(1), "the category itself still imports");
    check(f.resolve_nexus_id(1) == 0,
          "with assign off there is deliberately no mapping to resolve");
  }

  // --- Replace drops the mapping too: the table it points into is gone. ---
  {
    Factory &f = Factory::instance();
    f.activate_game("nexusreplacegame");
    f.import_external({{1, "Audio", 0}}, Factory::ImportMode::Merge);
    check(f.resolve_nexus_id(1) == 1, "the mapping is there before the replace");
    f.import_external({{1, "Audio", 0}}, Factory::ImportMode::Replace);
    check(f.resolve_nexus_id(1) == 1,
          "Replace re-adds the category and its mapping from the incoming list");
  }

  // --- A game switch clears the mappings: a Nexus id is per game, so carrying
  // one game's mapping into another's categories is the cross-game leak
  // activate_game() exists to stop. ---
  {
    Factory &f = Factory::instance();
    f.activate_game("nexusresolgame");
    f.import_external({{1, "Animations", 0}}, Factory::ImportMode::Merge);
    check(f.resolve_nexus_id(1) == 1, "the mapping is there before the switch");
    f.activate_game("someothergame");
    check(f.resolve_nexus_id(1) == 0,
          "switching games drops the mapping along with the category set");
  }
}