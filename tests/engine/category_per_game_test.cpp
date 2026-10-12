// Categories are per game, and that is the bug this file exists for.
//
// The failure it pins: every game plugin is dlopen'd into the same process
// whether or not that game is on screen (PluginLoader::load_directory walks
// the one plugin directory), and the ABI register_categories callback used to
// merge a game's declared categories straight into the single shared
// Category::Factory. So whichever game registered last or first decided what
// every instance saw, and a Skyrim instance showed The Binding of Isaac's
// categories. Registering is now recorded against the declaring game_id, and
// Category::Factory::activate_game() is the only thing that puts a set on
// screen.
#include "engine/plugin_host/category_factory.h"
#include "engine/mod/meta/category_set_registry.h"

#include <catch2/catch_test_macros.hpp>

namespace {
void check(bool cond, const char *what) {
  INFO(what);
  REQUIRE(cond);
}

// The two shapes that collided in production: Isaac's ids start at 1000 and
// its plugin declares them; the Bethesda set is Core's and uses 1..58.
void register_isaac(engine::Category::Factory &f) {
  static int ids[]           = {1000, 1001, 1006};
  static const char *names[] = {"Items", "Active Items", "Lua"};
  static int parents[]       = {0, 1000, 0};
  f.merge("thebindingofisaacrebirth", ids, names, parents, 3);
}
}  // namespace

TEST_CASE("category sets are per game, not global", "[engine][categories]") {
  using engine::Category::Factory;

  Factory &f = Factory::instance();

  // --- Both games' plugins are loaded, exactly as the loader does it. ---
  register_isaac(f);
  check(f.pluginCategoriesByGame().count("thebindingofisaacrebirth"),
        "Isaac's registration is recorded against Isaac");

  // --- A Skyrim instance comes up. It must not inherit Isaac's list. ---
  f.activate_game("SkyrimSpecialEdition", "Bethesda");
  check(!f.categoryExists(1000), "a Skyrim instance does not show Isaac Items");
  check(!f.categoryExists(1001), "a Skyrim instance does not show Isaac Active Items");
  check(!f.categoryExists(1006), "a Skyrim instance does not show Isaac Lua");
  check(f.categoryExists(1) && f.categoryById(1)->name == "Animations",
        "the Skyrim instance shows the Bethesda set instead");
  check(f.pluginCategories().empty(),
        "a game that declared no categories has no plugin categories of its own");

  // --- Switch to Isaac: now, and only now, do Isaac's categories appear. ---
  f.activate_game("thebindingofisaacrebirth");
  check(f.categoryExists(1000), "the Isaac instance shows its own Items");
  check(f.categoryById(1001)->parent_id == 1000,
        "Isaac's parent links survive the switch");
  check(!f.categoryExists(1),
        "and the Bethesda set did not leak the other way this time");

  // --- Switching back is not one-way: each game is recomputed, not appended.  ---
  f.activate_game("SkyrimSpecialEdition", "Bethesda");
  check(!f.categoryExists(1000), "Isaac's categories are gone again on the way back");
  check(f.categoryExists(1), "Bethesda is restored on the way back");

  // --- A game nobody knows anything about gets the empty set, not a neighbour's. ---
  f.activate_game("somegamewedonotshipapluginfor");
  check(f.categories().empty(),
        "an unknown game gets no categories rather than another game's");
}