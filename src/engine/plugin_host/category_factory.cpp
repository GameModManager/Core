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
                                       Factory::ImportMode mode) {
  // Replace clears the active set only. The per-game plugin registrations are
  // input to activate_game(), not part of the active set, so an import cannot
  // touch them and a later activate_game() still finds the game's own
  // categories intact.
  if (mode == Factory::ImportMode::Replace)
    categories_.clear();

  int added = 0;
  for (const auto &cat : cats) {
    if (cat.id == 0 || categories_.count(cat.id))
      continue;
    Entry entry;
    entry.id   = cat.id;
    entry.name = cat.name;
    // A parent that is not itself in the set would render as a dangling row in
    // every tree that walks the map, so it degrades to a root rather than
    // importing a link to nothing.
    entry.parent_id = categories_.count(cat.parent_id) ? cat.parent_id : 0;
    categories_.emplace(cat.id, std::move(entry));
    ++added;
  }
  if (added)
    rebuildTree();
  return added;
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
