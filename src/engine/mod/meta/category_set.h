#pragma once

#include <string>
#include <vector>

namespace engine {

// A single category within a named core set. Pure data — no behavior.
// Mirrors the fields the CategoryFactory stores (id, name, parent_id) so a set
// can be applied to the factory without transformation.
struct CategorySetEntry {
    int id = 0;
    std::string name;
    int parent_id = 0;  // 0 = root
};

// A named, reusable category set (e.g. "Bethesda", "Isaac", "Default").
// Source-of-truth for a game-agnostic or game-specific category hierarchy that
// plugins can opt into by name instead of registering every category in code.
struct CategorySetDefinition {
    std::string set_name;      // lookup key (e.g. "Bethesda", "Isaac")
    std::string display_name;  // human-readable (e.g. "Nexus Bethesda Categories")
    std::string description;   // optional one-liner
    std::vector<CategorySetEntry> categories;
};

}  // namespace engine
