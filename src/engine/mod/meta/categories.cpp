#include "engine/mod/meta/categories.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace engine {

namespace {

// Splits on '|' and trims each cell. Returns false when the row does not have
// at least `min_cells` non-empty cells.
bool split_row(const std::string& line, int min_cells,
               std::vector<std::string>& out) {
    out.clear();
    std::stringstream ss(line);
    std::string cell;
    while (std::getline(ss, cell, '|')) {
        size_t b = cell.find_first_not_of(" \t\r");
        size_t e = cell.find_last_not_of(" \t\r");
        if (b == std::string::npos) {
            out.emplace_back();
        } else {
            out.push_back(cell.substr(b, e - b + 1));
        }
    }
    return static_cast<int>(out.size()) >= min_cells;
}

std::vector<std::string> read_lines(const std::filesystem::path& path) {
    std::vector<std::string> lines;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

}  // namespace

void Categories::seed_default() {
    categories_.clear();
    nexus_map_.clear();

    // MO2's loadDefaultCategories() — order defines the combo-box order.
    // Entry order (and the duplicate id 39 for "Voice"/"Tattoos") is preserved
    // exactly as MO2 produces it, so files round-trip byte-identically.
    struct Def {
        int id;
        const char* name;
        int parent;
    };
    static constexpr Def kDefaults[] = {
        {1, "Animations", 0},        {52, "Poses", 1},
        {2, "Armour", 0},            {53, "Power Armor", 2},
        {3, "Audio", 0},             {38, "Music", 0},
        {39, "Voice", 0},            {5, "Clothing", 0},
        {41, "Jewelry", 5},          {42, "Backpacks", 5},
        {6, "Collectables", 0},      {28, "Companions", 0},
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
    for (const auto& d : kDefaults)
        categories_.push_back({d.id, d.name, {}, d.parent, false});

    rebuild_tree();
}

Categories Categories::load(const std::filesystem::path& instance_root) {
    Categories cats;  // seeded; replaced below when the file exists
    const auto cat_path = instance_root / "categories.dat";
    if (std::filesystem::exists(cat_path)) {
        cats.categories_.clear();
        cats.nexus_map_.clear();
        cats.load_from_lines(read_lines(cat_path));
        cats.load_nexus_map(read_lines(instance_root / "nexuscatmap.dat"));
        cats.rebuild_tree();
    }
    return cats;
}

void Categories::save(const std::filesystem::path& instance_root) const {
    std::error_code ec;
    std::filesystem::create_directories(instance_root, ec);

    std::ofstream cat_file(instance_root / "categories.dat");
    for (const auto& c : categories_) {
        if (c.id == 0) continue;  // "None" is implicit (MO2 skips it)
        cat_file << c.id << '|' << c.name << '|' << c.parent_id << '\n';
    }

    std::ofstream map_file(instance_root / "nexuscatmap.dat");
    for (const auto& [nexus_id, cat] : nexus_map_)
        map_file << cat.category_id << '|' << cat.name << '|' << nexus_id
                 << '\n';
}

bool Categories::contains(int id) const {
    return find(id) != nullptr;
}

const Categories::Category* Categories::find(int id) const {
    const Category* found = nullptr;
    for (const auto& c : categories_)
        if (c.id == id) found = &c;  // last wins, like MO2's ID map
    return found;
}

std::vector<const Categories::Category*> Categories::children_of(
    int parent_id) const {
    std::vector<const Category*> out;
    for (const auto& c : categories_)
        if (c.parent_id == parent_id && c.id != parent_id)
            out.push_back(&c);
    return out;
}

bool Categories::has_nexus(int nexus_id) const {
    return nexus_map_.find(nexus_id) != nexus_map_.end();
}

const Categories::NexusCat* Categories::nexus(int nexus_id) const {
    const auto it = nexus_map_.find(nexus_id);
    return it == nexus_map_.end() ? nullptr : &it->second;
}

const Categories::Category* Categories::category_for_nexus(
    int nexus_id) const {
    const auto* nc = nexus(nexus_id);
    if (!nc) return nullptr;
    return find(nc->category_id);
}

void Categories::add_category(int id, const std::string& name,
                              int parent_id) {
    categories_.push_back({id, name, {}, parent_id, false});
    rebuild_tree();
}

void Categories::add_nexus_mapping(int internal_id, const std::string& name,
                                   int nexus_id) {
    // nexus_map_ is the single source of truth for Nexus -> internal mapping
    // (MO2 keeps it in nexuscatmap.dat, separate from the category list).
    NexusCat cat;
    cat.name = name;
    cat.nexus_id = nexus_id;
    cat.category_id = internal_id;
    nexus_map_[nexus_id] = std::move(cat);
}

void Categories::remove_category(int id) {
    categories_.erase(
        std::remove_if(categories_.begin(), categories_.end(),
                       [id](const Category& c) { return c.id == id; }),
        categories_.end());
    rebuild_tree();
}

void Categories::set_parent(int id, int parent_id) {
    for (auto& c : categories_)
        if (c.id == id) c.parent_id = parent_id;
    rebuild_tree();
}

void Categories::rebuild_tree() {
    // Drop categories whose parent doesn't exist (parent 0 is always valid).
    std::unordered_map<int, bool> known;
    known[0] = true;
    for (const auto& c : categories_) known[c.id] = true;
    categories_.erase(
        std::remove_if(categories_.begin(), categories_.end(),
                       [&](const Category& c) {
                           return c.id != 0 && !known.count(c.parent_id);
                       }),
        categories_.end());

    update_has_children();
}

void Categories::update_has_children() {
    for (auto& c : categories_) c.has_children = false;
    for (const auto& c : categories_)
        for (auto& o : categories_)
            if (o.id == c.parent_id && c.id != c.parent_id) {
                o.has_children = true;
                break;
            }
}

void Categories::load_from_lines(const std::vector<std::string>& lines) {
    std::unordered_map<int, int> last_index;  // id -> vector index
    for (const auto& line : lines) {
        if (line.empty() || line[0] == '#') continue;

        std::vector<std::string> cells;
        if (!split_row(line, 3, cells)) continue;

        int id = 0, parent = 0;
        try {
            id = std::stoi(cells[0]);
            parent = std::stoi(cells[cells.size() - 1]);
        } catch (...) {
            continue;
        }

        Category cat;
        cat.id = id;
        cat.name = cells[1];
        cat.parent_id = parent;

        // 4-cell form: id|name|nexusIds|parentId
        if (cells.size() == 4 && !cells[2].empty()) {
            std::stringstream ns(cells[2]);
            std::string nid;
            while (std::getline(ns, nid, ',')) {
                try {
                    cat.nexus_ids.push_back(std::stoi(nid));
                } catch (...) {
                }
            }
        }
        categories_.push_back(std::move(cat));
    }
}

void Categories::load_nexus_map(const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> cells;
        if (!split_row(line, 3, cells)) continue;
        try {
            NexusCat cat;
            cat.category_id = std::stoi(cells[0]);
            cat.name = cells[1];
            cat.nexus_id = std::stoi(cells[2]);
            nexus_map_[cat.nexus_id] = std::move(cat);
        } catch (...) {
        }
    }
}

}  // namespace engine
