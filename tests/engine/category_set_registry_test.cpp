// Engine test for the named core category set registry.
//
// Covers: the singleton registry, built-in sets ("Default", "Bethesda",
// "Isaac"), find()/has() semantics (nullptr for unknown), and runtime
// register_set() (including replace-by-name).
#include "engine/mod/meta/category_set_registry.h"

#include <catch2/catch_test_macros.hpp>

namespace {

void require(bool cond, const char* msg) {
  INFO(msg);
  REQUIRE(cond);
}

} // namespace

TEST_CASE("category_set_registry", "[engine]") {
  using engine::CategorySetRegistry;

  CategorySetRegistry& reg = CategorySetRegistry::instance();

  // --- Built-in "Default" set exists and is empty (fallback). ---
  require(reg.has("Default"), "Default set is registered");
  const auto* def = reg.find("Default");
  require(def != nullptr, "find(Default) returns a definition");
  require(def->set_name == "Default", "Default set_name matches");
  require(def->categories.empty(), "Default set has no categories (user TBD)");

  // --- Built-in "Bethesda" set: MO2/Nexus categories. ---
  // MO2's default list has 57 entries; id 39 is used twice (Voice at root and
  // Tattoos under Body/Face/Hair), so the vector stores 57 entries.
  require(reg.has("Bethesda"), "Bethesda set is registered");
  const auto* bet = reg.find("Bethesda");
  require(bet != nullptr, "find(Bethesda) returns a definition");
  require(bet->display_name == "Nexus Bethesda Categories",
          "Bethesda display_name is the Nexus label");
  require(bet->categories.size() == 57, "Bethesda set has 57 entries (MO2 list)");
  // Spot-check a couple of entries and the parent hierarchy.
  bool found_animations = false, found_voice_child = false;
  for (const auto& e : bet->categories) {
    if (e.id == 1 && e.name == "Animations" && e.parent_id == 0)
      found_animations = true;
    if (e.id == 27 && e.name == "Combat" && e.parent_id == 9)
      found_voice_child = true;
  }
  require(found_animations, "Bethesda contains Animations (root)");
  require(found_voice_child, "Bethesda contains Combat under Gameplay (9)");

  // --- Built-in "Isaac" set: 22 Steam Workshop categories. ---
  require(reg.has("Isaac"), "Isaac set is registered");
  const auto* isaac = reg.find("Isaac");
  require(isaac != nullptr, "find(Isaac) returns a definition");
  require(isaac->display_name == "TheBindingOfIsaac Categories",
          "Isaac display_name matches");
  require(isaac->categories.size() == 22, "Isaac set has 22 categories");
  bool found_items = false, found_active_child = false;
  for (const auto& e : isaac->categories) {
    if (e.id == 1000 && e.name == "Items" && e.parent_id == 0)
      found_items = true;
    if (e.id == 1001 && e.name == "Active Items" && e.parent_id == 1000)
      found_active_child = true;
  }
  require(found_items, "Isaac contains Items (root)");
  require(found_active_child, "Isaac contains Active Items under Items (1000)");

  // --- find() returns nullptr for unknown names. ---
  require(!reg.has("DoesNotExist"), "unknown set is not present");
  require(reg.find("DoesNotExist") == nullptr,
          "find(unknown) returns nullptr");

  // --- Runtime register_set() adds a new set. ---
  engine::CategorySetDefinition custom;
  custom.set_name = "CustomGame";
  custom.display_name = "Custom Game Categories";
  custom.description = "A game-specific set registered at runtime.";
  custom.categories = {{1, "Root", 0}, {2, "Child", 1}};
  reg.register_set(std::move(custom));
  require(reg.has("CustomGame"), "register_set adds a new set");
  const auto* c = reg.find("CustomGame");
  require(c && c->categories.size() == 2, "registered set has its categories");

  // --- register_set() replaces an existing set by name. ---
  engine::CategorySetDefinition override;
  override.set_name = "CustomGame";
  override.categories = {{9, "Only", 0}};
  reg.register_set(std::move(override));
  const auto* o = reg.find("CustomGame");
  require(o && o->categories.size() == 1 && o->categories[0].id == 9,
          "register_set replaces an existing set by name");
}
