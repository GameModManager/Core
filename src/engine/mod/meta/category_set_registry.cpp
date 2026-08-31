#include "engine/mod/meta/category_set_registry.h"

namespace engine {

CategorySetRegistry& CategorySetRegistry::instance() {
    static CategorySetRegistry s;
    return s;
}

CategorySetRegistry::CategorySetRegistry() {
    register_builtin_sets();
}

void CategorySetRegistry::register_set(CategorySetDefinition set) {
    sets_[set.set_name] = std::move(set);
}

const CategorySetDefinition* CategorySetRegistry::find(const std::string& set_name) const {
    auto it = sets_.find(set_name);
    return it == sets_.end() ? nullptr : &it->second;
}

bool CategorySetRegistry::has(const std::string& set_name) const {
    return sets_.find(set_name) != sets_.end();
}

void CategorySetRegistry::register_builtin_sets() {
    // --- "Default" - empty fallback for plugins that declare nothing. ---
    // The user-provided universal category list will populate this set later;
    // until then it seeds nothing so the factory stays empty for unknown games.
    {
        CategorySetDefinition def;
        def.set_name = "Default";
        def.display_name = "Default Categories";
        def.description = "Empty fallback set for games with no declared core set.";
        register_set(std::move(def));
    }

    // --- "Bethesda" - MO2/Nexus Bethesda (Skyrim-style) 58-category list. ---
    // Source-of-truth moved here from the old Categories::seed_default().
    // IDs are the classic MO2 ids (1..58). Note: MO2's list historically
    // repeats id 39 ("Voice" then "Tattoos"); the factory dedupes by id, so
    // only the first (Voice) survives - matching the prior register_categories
    // behavior. Kept verbatim for fidelity with MO2's category file format.
    {
        static const CategorySetEntry kBethesda[] = {
            {1, "Animations", 0},        {52, "Poses", 1},
            {2, "Armour", 0},            {53, "Power Armor", 2},
            {3, "Audio", 0},             {38, "Music", 0},
            {39, "Voice", 0},            {5, "Clothing", 0},
            {41, "Jewelry", 5},          {42, "Backpacks", 5},
            {6, "Collectables", 0},       {28, "Companions", 0},
            {7, "Creatures, Mounts, & Vehicles", 0}, {8, "Factions", 0},
            {9, "Gameplay", 0},          {27, "Combat", 9},
            {43, "Crafting", 9},         {48, "Overhauls", 9},
            {49, "Perks", 9},            {54, "Radio", 9},
            {55, "Shouts", 9},           {22, "Skills & Levelling", 9},
            {58, "Weather & Lighting", 9}, {44, "Equipment", 43},
            {45, "Home/Settlement", 43}, {10, "Body, Face, & Hair", 0},
            {39, "Tattoos", 10},         {40, "Character Presets", 0},
            {11, "Items", 0},            {32, "Mercantile", 0},
            {37, "Ammo", 11},            {19, "Weapons", 11},
            {36, "Weapon & Armour Sets", 11}, {23, "Player Homes", 0},
            {25, "Castles & Mansions", 23}, {51, "Settlements", 23},
            {12, "Locations", 0},        {4, "Cities", 12},
            {31, "Landscape Changes", 0}, {29, "Environment", 0},
            {30, "Immersion", 0},        {20, "Magic", 0},
            {21, "Models & Textures", 0}, {33, "Modders resources", 0},
            {13, "NPCs", 0},             {24, "Bugfixes", 0},
            {14, "Patches", 24},         {35, "Utilities", 0},
            {26, "Cheats", 0},           {15, "Quests", 0},
            {16, "Races & Classes", 0},  {34, "Stealth", 0},
            {17, "UI", 0},               {18, "Visuals", 0},
            {50, "Pip-Boy", 18},         {46, "Shader Presets", 0},
            {47, "Miscellaneous", 0},
        };
        CategorySetDefinition def;
        def.set_name = "Bethesda";
        def.display_name = "Nexus Bethesda Categories";
        def.description = "Classic MO2/Nexus Bethesda (Skyrim-style) category hierarchy.";
        def.categories.assign(kBethesda, kBethesda + sizeof(kBethesda) / sizeof(kBethesda[0]));
        register_set(std::move(def));
    }

    // --- "Isaac" - The Binding of Isaac: Rebirth Steam Workshop categories. ---
    // Moved here from the Isaac plugin (previously 22 register_categories calls).
    // IDs 1000..1021, parent hierarchy as in the plugin.
    {
        static const CategorySetEntry kIsaac[] = {
            {1000, "Items", 0},
            {1001, "Active Items", 1000},
            {1002, "Trinkets", 1000},
            {1003, "Pills", 1000},
            {1004, "Cards", 1000},
            {1005, "Pickups", 1000},
            {1006, "Lua", 0},
            {1007, "Rooms", 0},
            {1008, "Floors", 1007},
            {1009, "Player Characters", 0},
            {1010, "Familiars", 0},
            {1011, "Babies", 0},
            {1012, "Enemies", 0},
            {1013, "Graphics", 0},
            {1014, "Shaders", 1013},
            {1015, "Sound Effects", 0},
            {1016, "Music", 1015},
            {1017, "Bosses", 0},
            {1018, "Hazards", 0},
            {1019, "Challenges", 0},
            {1020, "Tweaks", 0},
            {1021, "Removals", 0},
        };
        CategorySetDefinition def;
        def.set_name = "Isaac";
        def.display_name = "TheBindingOfIsaac Categories";
        def.description = "The Binding of Isaac: Rebirth Steam Workshop category hierarchy.";
        def.categories.assign(kIsaac, kIsaac + sizeof(kIsaac) / sizeof(kIsaac[0]));
        register_set(std::move(def));
    }
}

}  // namespace engine
