#pragma once

#include <string>
#include <vector>

namespace engine::Source::Nexus {

// One category as Nexus publishes it for a game.
//
// The field names and types come from the NexusMods.App DTO the app itself
// uses (references/NexusMods.App/src/NexusMods.Abstractions.NexusWebApi/
// DTOs/Category.cs): "category_id", "name", "parent_category".
//
// CATEGORY IDS ARE PER-GAME. That DTO says so in as many words - "Mod
// categories. Unique per game. i.e. Each game has its own set of categories."
// A category id is therefore only meaningful alongside the game domain it
// came from, which is why fetch_categories() is always asked for a domain and
// never returns a global list.
struct NexusCategory {
  int id = 0;
  std::string name;
  int parent_id = 0;  // 0 = root
};

// The categories of one game domain.
struct CategoryList {
  bool ok = false;  // false when the body was unparseable or held no categories
  std::vector<NexusCategory> categories;
};

// Pure parser for a categories.json body, extracted so the mapping is
// unit-testable without the network - same split as parse_mod_info() and
// parse_file_list(). Never throws; ok=false on anything unparseable.
//
// "parent_category" is deliberately fussy: Nexus sends either the owning
// category's id or the literal false for a root category, so both shapes have
// to be accepted and anything else means that row is not usable.
CategoryList parse_categories(const std::string &body);

// Fetches the categories of one Nexus game domain (e.g. "skyrimspecialedition").
// Needs a configured API key; ok=false without one, on an HTTP error, or on an
// unparseable body. Never throws.
CategoryList fetch_categories(const std::string &nexus_domain);

}  // namespace engine::Source::Nexus
