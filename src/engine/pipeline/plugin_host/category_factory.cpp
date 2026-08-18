#include "engine/pipeline/plugin_host/category_factory.h"

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

} // namespace

void CategoryFactory::load(const std::filesystem::path &path) {
  std::ifstream in(path);
  if (!in)
    return; // missing/unreadable file: keep the current set

  std::map<int, Category> loaded;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty() || line[0] == '#')
      continue;

    std::vector<std::string> cells;
    if (!split_row(line, 3, cells))
      continue;

    int id = 0;
    int parent = 0;
    try {
      id = std::stoi(cells[0]);
      // Last cell is the parent id, so the 4-cell nexus variant
      // (id|name|nexusIds|parentId) parses the same way.
      parent = std::stoi(cells[cells.size() - 1]);
    } catch (...) {
      continue; // malformed row: skip
    }
    if (id == 0)
      continue; // "None" is implicit

    Category cat;
    cat.id = id;
    cat.name = cells[1];
    cat.parent_id = parent;
    loaded[id] = std::move(cat);
  }

  categories_ = std::move(loaded);
  rebuildTree();
}

void CategoryFactory::save(const std::filesystem::path &path) const {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);

  std::ofstream out(path);
  for (const auto &[id, cat] : categories_) {
    if (id == 0)
      continue; // "None" is implicit
    out << id << '|' << cat.name << '|' << cat.parent_id << '\n';
  }
}

void CategoryFactory::merge(const int *ids, const char *const *names,
                            const int *parent_ids, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    if (!ids || !names)
      continue;
    int id = ids[i];
    if (categories_.count(id))
      continue; // skip duplicate
    Category cat;
    cat.id = id;
    cat.name = names[i] ? names[i] : "";
    cat.parent_id = (parent_ids && parent_ids[i]) ? parent_ids[i] : 0;
    categories_.emplace(id, std::move(cat));
  }
  rebuildTree();
}

bool CategoryFactory::categoryExists(int id) const {
  return categories_.count(id) > 0;
}

const CategoryFactory::Category *CategoryFactory::categoryById(int id) const {
  auto it = categories_.find(id);
  return it != categories_.end() ? &it->second : nullptr;
}

void CategoryFactory::addCategory(int id, const std::string &name,
                                  int parent_id) {
  if (id == 0 || categories_.count(id))
    return; // "None" is implicit; duplicates are skipped
  Category cat;
  cat.id = id;
  cat.name = name;
  cat.parent_id = parent_id;
  categories_.emplace(id, std::move(cat));
  rebuildTree();
}

void CategoryFactory::removeCategory(int id) {
  if (!categories_.erase(id))
    return;
  // Re-parent direct children to root so the tree stays valid.
  for (auto &entry : categories_)
    if (entry.second.parent_id == id)
      entry.second.parent_id = 0;
  rebuildTree();
}

void CategoryFactory::updateCategory(int id, const std::string &name,
                                     int parent_id) {
  auto it = categories_.find(id);
  if (it == categories_.end())
    return;
  it->second.name = name;
  it->second.parent_id = parent_id;
  rebuildTree();
}

void CategoryFactory::rebuildTree() { updateHasChildren(); }

void CategoryFactory::updateHasChildren() {
  for (auto &entry : categories_)
    entry.second.hasChildren = false;
  for (const auto &entry : categories_)
    if (entry.second.parent_id != 0) {
      auto it = categories_.find(entry.second.parent_id);
      if (it != categories_.end())
        it->second.hasChildren = true;
    }
}

} // namespace engine