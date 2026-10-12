#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace engine {

namespace Category {

  // Category registry. One active set at a time, but plugin-declared sets are
  // kept PER GAME: the ABI register_categories callback says which game a
  // registration belongs to, and activate_game() is the only thing that puts
  // one game's categories in front of the user. That split is the whole point
  // - the factory used to merge every game's plugin registrations into one
  // shared map at plugin-load time, so an instance of one game showed another
  // game's categories. Parent ID 0 = root.
  // Persisted to categories.dat as pipe-delimited rows: ID|Name|ParentID.
  class Factory {
  public:
    struct Entry {
      int id = 0;
      std::string name;
      int parent_id    = 0;
      bool hasChildren = false;  // computed by rebuildTree()
    };

    static Factory &instance() {
      static Factory s;
      return s;
    }

    // --- File I/O (categories.dat: ID|Name|ParentID per line) ---
    // Replaces the current set. A missing/unreadable file leaves the registry
    // unchanged. Never throws.
    void load(const std::filesystem::path &path);
    void save(const std::filesystem::path &path) const;

    // Records categories declared by one game plugin (ABI register_categories).
    // Stored under game_id and NOT pushed into the active set: the active set
    // only ever changes through activate_game(), which is what keeps one
    // game's categories out of another game's instance. Duplicates by ID are
    // skipped. parent_id 0 = root category.
    void merge(const std::string &game_id, const int *ids, const char *const *names,
               const int *parent_ids, size_t count);

    // Makes one game's categories the active set: drops whatever was active,
    // then layers this game's plugin-declared categories and its core set (or
    // "Default" when core_set_name is empty) on top. Every switch between
    // games goes through here.
    void activate_game(const std::string &game_id,
                       const std::string &core_set_name = "");

    // Populates the factory from a named core set (see CategorySetRegistry).
    // Existing categories are NOT cleared - the set adds missing categories only
    // (additive). Returns true when the set was found and applied.
    bool applyCoreSet(const std::string &set_name);

    // What an import does with the categories already in the active set. These
    // are MO2's own two strategies from CategoryImportDialog
    // (references/modorganizer/src/categoryimportdialog.cpp:37-45).
    enum class ImportMode {
      Merge,    // keep what is active, add only ids that are missing
      Replace,  // drop the active set first, as MO2's "Overwrite" does
    };

    // Imports an externally-defined category list - a Nexus game's fetched
    // categories - into the active set. Entry is reused rather than a second
    // near-identical row type: {id, name, parent_id} is already exactly that.
    //
    // Nexus ids are per game, so the caller must only ever pass the list for
    // the game the active set belongs to (fetch_categories takes a domain for
    // the same reason). Returns the number of categories actually added; ids
    // already present are never overwritten, so a second import of the same
    // list is a no-op rather than a duplicate.
    int import_external(const std::vector<Entry> &cats, ImportMode mode);

    // --- Lookups ---
    [[nodiscard]] bool categoryExists(int id) const;
    [[nodiscard]] const Entry *categoryById(int id) const;
    [[nodiscard]] const std::map<int, Entry> &categories() const { return categories_; }

    // --- Mutation ---
    // Adds a new category; no-op when the id already exists (or id == 0).
    void addCategory(int id, const std::string &name, int parent_id = 0);
    // Removes a category and re-parents its direct children to root (0).
    void removeCategory(int id);
    // Updates the name and parent of an existing category; no-op when missing.
    void updateCategory(int id, const std::string &name, int parent_id);
    // Drops every active entry. The per-game plugin registrations survive -
    // they are input to activate_game(), not part of the active set.
    void clear();

    // The active game's plugin-declared categories, as activate_game() laid
    // them down. Empty for a game that declared none.
    [[nodiscard]] const std::map<int, Entry> &pluginCategories() const {
      return plugin_categories_;
    }

    // Every game's plugin-declared categories, keyed by game_id.
    [[nodiscard]] const std::map<std::string, std::map<int, Entry>> &
    pluginCategoriesByGame() const {
      return plugin_categories_by_game_;
    }

    // Recomputes the hasChildren flags after any structural change.
    void rebuildTree();

  private:
    Factory() = default;
    void updateHasChildren();

    std::map<int, Entry> categories_;
    std::map<int, Entry> plugin_categories_;  // the active game's, mirrored
    std::map<std::string, std::map<int, Entry>> plugin_categories_by_game_;
  };

}  // namespace Category

// Backward-compatible alias for existing consumers
using CategoryFactory = Category::Factory;

}  // namespace engine
