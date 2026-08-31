#include "engine/mod/meta/categories.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace engine {

namespace {

// Splits on '|' and trims each cell. Returns false when the row does not have
// at least `min_cells` non-empty cells.
bool split_row(const std::string &line, int min_cells,
               std::vector<std::string> &out) {
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

std::vector<std::string> read_lines(const std::filesystem::path &path) {
  std::vector<std::string> lines;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    lines.push_back(line);
  }
  return lines;
}

} // namespace

void Categories::seed_default() {
  categories_.clear();
  nexus_map_.clear();

  // Default category set - universal, game-agnostic categories merged from
  // Bethesda/MO2 and Isaac sets, with additional categories drawn from real
  // modding load orders. IDs 1000+ avoid collisions with legacy per-game sets.
  struct Def {
    int id;
    const char *name;
    int parent;
  };
  static constexpr Def kDefaults[] = {
      // --- Gameplay (1000) ---
      {1000, "Gameplay", 0},
      {1001, "Combat", 1000},
      {1002, "Skills & Leveling", 1000},
      {1003, "Quests", 1000},
      {1004, "Magic & Abilities", 1000},
      {1005, "Stealth", 1000},
      {1006, "Crafting", 1000},
      {1007, "AI & Behavior", 1000},
      {1008, "Difficulty & Balance", 1000},
      {1009, "Overhauls", 1000},
      {1010, "Immersion", 1000},
      {1011, "Quality of Life", 1000},
      {1012, "New Mechanics", 1000},

      // --- Animation & Physics (1050) ---
      {1050, "Animation & Physics", 0},
      {1051, "Animation Replacers", 1050},
      {1052, "Animation Frameworks", 1050},
      {1053, "Movement", 1050},
      {1054, "Physics", 1050},
      {1055, "Paired Animations", 1050},

      // --- Items & Equipment (1100) ---
      {1100, "Items & Equipment", 0},
      {1101, "Weapons", 1100},
      {1102, "Armour & Shields", 1100},
      {1103, "Clothing & Accessories", 1100},
      {1104, "Consumables", 1100},
      {1105, "Items & Collectibles", 1100},
      {1106, "Furniture & Structures", 1100},
      {1107, "Weapon & Armour Sets", 1100},
      {1108, "Equipment Overhaul", 1100},

      // --- Body & Character Systems (1150) ---
      {1150, "Body & Character Systems", 0},
      {1151, "Body Models", 1150},
      {1152, "Character Creation", 1150},
      {1153, "Body Sliders & Presets", 1150},
      {1154, "Tattoos & Marks", 1150},
      {1155, "Skeleton & Rig", 1150},

      // --- Characters & NPCs (1200) ---
      {1200, "Characters & NPCs", 0},
      {1201, "Companions & Followers", 1200},
      {1202, "Enemies & Creatures", 1200},
      {1203, "NPCs", 1200},
      {1204, "Factions", 1200},

      // --- World & Environment (1300) ---
      {1300, "World & Environment", 0},
      {1301, "Locations", 1300},
      {1302, "Cities & Towns", 1300},
      {1303, "Landscape & Terrain", 1300},
      {1304, "Flora & Vegetation", 1300},
      {1305, "Water & Ice", 1300},
      {1306, "Weather & Lighting", 1300},
      {1307, "Environment", 1300},
      {1308, "Player Homes", 1300},
      {1309, "World Maps", 1300},
      {1310, "Shrines & Statues", 1300},
      {1311, "Interiors", 1300},

      // --- Visuals (1400) ---
      {1400, "Visuals", 0},
      {1401, "Models & Textures", 1400},
      {1402, "Food & Ingredients", 1400},
      {1403, "Potions & Consumables", 1400},
      {1404, "Clutter & Miscellaneous", 1400},
      {1405, "Shaders & Presets", 1400},
      {1406, "Visual Effects", 1400},
      {1407, "LOD", 1400},

      // --- Audio (1500) ---
      {1500, "Audio", 0},
      {1501, "Music", 1500},
      {1502, "Sound Effects", 1500},
      {1503, "Voice & Dialogue", 1500},

      // --- Interface (1600) ---
      {1600, "Interface", 0},
      {1601, "UI Overhaul", 1600},
      {1602, "HUD", 1600},
      {1603, "Menus", 1600},
      {1604, "Controls", 1600},
      {1605, "Camera", 1600},
      {1606, "Tooltips & Info", 1600},

      // --- Technical (1700) ---
      {1700, "Technical", 0},
      {1701, "Bug Fixes", 1700},
      {1702, "Patches & Compatibility", 1700},
      {1703, "Performance", 1700},
      {1704, "Utilities", 1700},
      {1705, "Cheats & Console", 1700},

      // --- Modding (1800) - sub of Technical ---
      {1800, "Modding", 1700},
      {1801, "Modders Resources", 1800},
      {1802, "Frameworks & Libraries", 1800},
      {1803, "Tutorials & Docs", 1800},
      {1804, "Modding Utilities", 1800},

      // --- File Support (1900) - file-format parsers & helpers ---
      {1900, "File Support", 0},
  };
  for (const auto &d : kDefaults)
    categories_.push_back({d.id, d.name, {}, d.parent, false});

  rebuild_tree();
}

Categories Categories::load(const std::filesystem::path &instance_root) {
  Categories cats; // seeded; replaced below when the file exists
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

void Categories::save(const std::filesystem::path &instance_root) const {
  std::error_code ec;
  std::filesystem::create_directories(instance_root, ec);

  std::ofstream cat_file(instance_root / "categories.dat");
  for (const auto &c : categories_) {
    if (c.id == 0)
      continue; // "None" is implicit (MO2 skips it)
    cat_file << c.id << '|' << c.name << '|' << c.parent_id << '\n';
  }

  std::ofstream map_file(instance_root / "nexuscatmap.dat");
  for (const auto &[nexus_id, cat] : nexus_map_)
    map_file << cat.category_id << '|' << cat.name << '|' << nexus_id << '\n';
}

bool Categories::contains(int id) const { return find(id) != nullptr; }

const Categories::Category *Categories::find(int id) const {
  const Category *found = nullptr;
  for (const auto &c : categories_)
    if (c.id == id)
      found = &c; // last wins, like MO2's ID map
  return found;
}

std::vector<const Categories::Category *>
Categories::children_of(int parent_id) const {
  std::vector<const Category *> out;
  for (const auto &c : categories_)
    if (c.parent_id == parent_id && c.id != parent_id)
      out.push_back(&c);
  return out;
}

bool Categories::has_nexus(int nexus_id) const {
  return nexus_map_.find(nexus_id) != nexus_map_.end();
}

const Categories::NexusCat *Categories::nexus(int nexus_id) const {
  const auto it = nexus_map_.find(nexus_id);
  return it == nexus_map_.end() ? nullptr : &it->second;
}

const Categories::Category *Categories::category_for_nexus(int nexus_id) const {
  const auto *nc = nexus(nexus_id);
  if (!nc)
    return nullptr;
  return find(nc->category_id);
}

void Categories::add_category(int id, const std::string &name, int parent_id) {
  categories_.push_back({id, name, {}, parent_id, false});
  rebuild_tree();
}

void Categories::add_nexus_mapping(int internal_id, const std::string &name,
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
                     [id](const Category &c) { return c.id == id; }),
      categories_.end());
  rebuild_tree();
}

void Categories::set_parent(int id, int parent_id) {
  for (auto &c : categories_)
    if (c.id == id)
      c.parent_id = parent_id;
  rebuild_tree();
}

void Categories::rebuild_tree() {
  // Drop categories whose parent doesn't exist (parent 0 is always valid).
  std::unordered_map<int, bool> known;
  known[0] = true;
  for (const auto &c : categories_)
    known[c.id] = true;
  categories_.erase(std::remove_if(categories_.begin(), categories_.end(),
                                   [&](const Category &c) {
                                     return c.id != 0 &&
                                            !known.count(c.parent_id);
                                   }),
                    categories_.end());

  update_has_children();
}

void Categories::update_has_children() {
  for (auto &c : categories_)
    c.has_children = false;
  for (const auto &c : categories_)
    for (auto &o : categories_)
      if (o.id == c.parent_id && c.id != c.parent_id) {
        o.has_children = true;
        break;
      }
}

void Categories::load_from_lines(const std::vector<std::string> &lines) {
  std::unordered_map<int, int> last_index; // id -> vector index
  for (const auto &line : lines) {
    if (line.empty() || line[0] == '#')
      continue;

    std::vector<std::string> cells;
    if (!split_row(line, 3, cells))
      continue;

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

void Categories::load_nexus_map(const std::vector<std::string> &lines) {
  for (const auto &line : lines) {
    if (line.empty() || line[0] == '#')
      continue;
    std::vector<std::string> cells;
    if (!split_row(line, 3, cells))
      continue;
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

} // namespace engine
