#include "engine/plugin_host/category_factory.h"
#include "engine/mod/meta/category_set_registry.h"

#include <fstream>
#include <sstream>
#include <vector>

namespace engine {

namespace {

  // Splits on '|' and trims each cell. Returns false when the row does not have
  // at least `min_cells` cells.
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

}  // namespace

void Category::Factory::load(const std::filesystem::path &path) {
  std::ifstream in(path);
  if (!in)
    return;  // missing/unreadable file: keep the current set

  std::map<int, Entry> loaded;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty() || line[0] == '#')
      continue;

    std::vector<std::string> cells;
    if (!split_row(line, 3, cells))
      continue;

    int id     = 0;
    int parent = 0;
    try {
      id = std::stoi(cells[0]);
      // Last cell is the parent id, so the 4-cell nexus variant
      // (id|name|nexusIds|parentId) parses the same way.
      parent = std::stoi(cells[cells.size() - 1]);
    } catch (...) {
      continue;  // malformed row: skip
    }
    if (id == 0)
      continue;  // "None" is implicit

    Entry cat;
    cat.id        = id;
    cat.name      = cells[1];
    cat.parent_id = parent;
    loaded[id]    = std::move(cat);
  }

  categories_ = std::move(loaded);
  nexus_map_.clear();

  // nexuscatmap.dat: `categoryId|name|nexusId` per line. A missing file simply
  // means no mappings, which is what a first run looks like; a malformed row is
  // skipped, as everywhere else in this file.
  {
    std::ifstream nexus_in(path.parent_path() / "nexuscatmap.dat");
    std::string nexus_line;
    while (std::getline(nexus_in, nexus_line)) {
      if (!nexus_line.empty() && nexus_line.back() == '\r')
        nexus_line.pop_back();
      if (nexus_line.empty() || nexus_line[0] == '#')
        continue;
      std::vector<std::string> cells;
      if (!split_row(nexus_line, 3, cells))
        continue;
      try {
        NexusMapping map;
        map.category_id = std::stoi(cells[0]);
        map.name        = cells[1];
        map.id          = std::stoi(cells[2]);
        if (map.id != 0)
          nexus_map_[map.id] = std::move(map);
      } catch (...) {
        continue;
      }
    }
  }

  rebuildTree();
}

void Category::Factory::save(const std::filesystem::path &path) const {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);

  std::ofstream out(path);
  for (const auto &[id, cat] : categories_) {
    if (id == 0)
      continue;  // "None" is implicit
    out << id << '|' << cat.name << '|' << cat.parent_id << '\n';
  }

  // The Nexus mapping lives in its own file beside categories.dat, not in a
  // fourth column of it: MO2's nexuscatmap.dat
  // (references/modorganizer/src/categories.cpp:48-50) written in the same
  // `categoryId|name|nexusId` order (:221-224).
  std::ofstream nexus_out(path.parent_path() / "nexuscatmap.dat");
  for (const auto &[nexus_id, map] : nexus_map_)
    nexus_out << map.category_id << '|' << map.name << '|' << nexus_id << '\n';
}

void Category::Factory::merge(const std::string &game_id, const int *ids,
                              const char *const *names, const int *parent_ids,
                              size_t count) {
  if (!ids || !names || count == 0)
    return;

  auto &declared = plugin_categories_by_game_[game_id];
  for (size_t i = 0; i < count; ++i) {
    Entry cat;
    cat.id        = ids[i];
    cat.name      = names[i] ? names[i] : "";
    cat.parent_id = (parent_ids && parent_ids[i]) ? parent_ids[i] : 0;
    // insert, not assign: a repeated id keeps the name it was first declared
    // with, which is the contract merge() has always had.
    declared.insert({cat.id, cat});
  }
}

void Category::Factory::activate_game(const std::string &game_id,
                                      const std::string &core_set_name) {
  categories_.clear();
  plugin_categories_.clear();
  // The Nexus mappings go with the set they point into: a Nexus category id is
  // per game (engine/source/nexus/categories.h:14-18), so carrying one game's
  // mappings across a switch would resolve it into another game's categories.
  nexus_map_.clear();

  // find(), never begin(): the map holds every game's registrations, and
  // begin() would hand back whichever game happens to sort first - which is
  // exactly the cross-game leak activate_game() exists to stop.
  const auto it = plugin_categories_by_game_.find(game_id);
  if (it != plugin_categories_by_game_.end())
    plugin_categories_ = it->second;

  // Plugin-declared categories first, so a core set cannot overwrite a game
  // plugin's own naming for an id the two happen to share.
  for (const auto &[id, cat] : plugin_categories_) {
    if (!categories_.count(id))
      categories_.emplace(id, cat);
  }

  applyCoreSet(core_set_name.empty() ? "Default" : core_set_name);
}

int Category::Factory::import_external(const std::vector<Entry> &cats,
                                       Factory::ImportMode mode, bool assign,
                                       bool remap) {
  // Replace clears the active set only. The per-game plugin registrations are
  // input to activate_game(), not part of the active set, so an import cannot
  // touch them and a later activate_game() still finds the game's own
  // categories intact.
  if (mode == Factory::ImportMode::Replace) {
    categories_.clear();
    nexus_map_.clear();
  }

  int added = 0;
  for (const auto &cat : cats) {
    if (cat.id == 0)
      continue;
    // MO2 matches on NAME, not on id: `table->findItems(name, ...)` over the
    // category table (references/modorganizer/src/categoriesdialog.cpp:330).
    // So a category the user already has under a different local id is NOT
    // duplicated - the Nexus id is attached to their row instead, which is
    // what makes "Remap existing mappings" worth anything.
    const Entry *existing = nullptr;
    for (const auto &entry : categories_) {
      if (entry.second.name == cat.name) {
        existing = &entry.second;
        break;
      }
    }

    if (!existing) {
      Entry entry;
      entry.id   = cat.id;
      entry.name = cat.name;
      // A parent that is not itself in the set would render as a dangling row
      // in every tree that walks the map, so it degrades to a root rather than
      // importing a link to nothing.
      entry.parent_id = categories_.count(cat.parent_id) ? cat.parent_id : 0;
      categories_.emplace(cat.id, std::move(entry));
      ++added;
      if (assign)
        assign_nexus_mapping(cat.id, cat.name, cat.id);
      continue;
    }
    // MO2's else branch (:341-354). Without remap the row is left exactly as it
    // is: an existing category is never rewritten by an import.
    if (remap)
      assign_nexus_mapping(cat.id, cat.name, existing->id);
  }
  if (added)
    rebuildTree();
  return added;
}

void Category::Factory::assign_nexus_mapping(int nexus_id, const std::string &name,
                                             int category_id) {
  NexusMapping map;
  map.id               = nexus_id;
  map.name             = name;
  map.category_id      = category_id;
  nexus_map_[nexus_id] = std::move(map);
}

int Category::Factory::resolve_nexus_id(int nexus_id) const {
  const auto it = nexus_map_.find(nexus_id);
  if (it == nexus_map_.end())
    return 0;  // unmapped: MO2 logs and returns 0 (categories.cpp:523-524)
  // The category the mapping named can be gone - removed in the dialog, or
  // dropped by an activate_game() that laid down a different game's set. MO2
  // checks m_IDMap for exactly this and returns 0 rather than a dangling
  // index (categories.cpp:517-521).
  return categories_.count(it->second.category_id) ? it->second.category_id : 0;
}

void Category::Factory::clear_nexus_mappings() {
  nexus_map_.clear();
}

bool Category::Factory::applyCoreSet(const std::string &set_name) {
  const auto *set = CategorySetRegistry::instance().find(set_name);
  if (!set)
    return false;

  for (const auto &entry : set->categories) {
    if (entry.id == 0)
      continue;
    if (!categories_.count(entry.id)) {
      Entry cat;
      cat.id        = entry.id;
      cat.name      = entry.name;
      cat.parent_id = entry.parent_id;
      categories_.emplace(entry.id, std::move(cat));
    }
  }
  rebuildTree();
  return true;
}

bool Category::Factory::categoryExists(int id) const {
  return categories_.count(id) > 0;
}

const Category::Factory::Entry *Category::Factory::categoryById(int id) const {
  auto it = categories_.find(id);
  return it != categories_.end() ? &it->second : nullptr;
}

void Category::Factory::addCategory(int id, const std::string &name, int parent_id) {
  if (id == 0 || categories_.count(id))
    return;  // "None" is implicit; duplicates are skipped
  Entry cat;
  cat.id        = id;
  cat.name      = name;
  cat.parent_id = parent_id;
  categories_.emplace(id, std::move(cat));
  rebuildTree();
}

void Category::Factory::removeCategory(int id) {
  if (!categories_.erase(id))
    return;
  // Re-parent direct children to root so the tree stays valid.
  for (auto &entry : categories_)
    if (entry.second.parent_id == id)
      entry.second.parent_id = 0;
  rebuildTree();
}

void Category::Factory::updateCategory(int id, const std::string &name, int parent_id) {
  auto it = categories_.find(id);
  if (it == categories_.end())
    return;
  it->second.name      = name;
  it->second.parent_id = parent_id;
  rebuildTree();
}

void Category::Factory::rebuildTree() {
  updateHasChildren();
}

void Category::Factory::clear() {
  categories_.clear();
  plugin_categories_.clear();
  nexus_map_.clear();
}

void Category::Factory::updateHasChildren() {
  for (auto &entry : categories_)
    entry.second.hasChildren = false;
  for (const auto &entry : categories_)
    if (entry.second.parent_id != 0) {
      auto it = categories_.find(entry.second.parent_id);
      if (it != categories_.end())
        it->second.hasChildren = true;
    }
}

}  // namespace engine
