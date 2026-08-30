#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace engine {

// Global instance registry that tracks ALL instances (installed and portable).
// Lives in the config directory as instance_registry.toml.
class InstanceRegistry {
public:
  // Registry entry — one per tracked instance
  struct Entry {
    std::string name;         // folder name (unique key)
    std::string root;         // absolute path to instance root
    std::string type;         // "installed" | "portable"
    std::string game_id;      // e.g. "SkyrimSpecialEdition"
    std::string display_name; // user-facing name
    std::string created_at;   // ISO 8601
    std::string last_used_at; // ISO 8601
    bool is_active = false;   // currently loaded?

    // Check if root directory + instance.toml exists
    [[nodiscard]] bool exists() const;
  };

  // Validation status for an entry
  enum class ValidationStatus {
    Valid,           // instance exists and is loadable
    MissingRoot,     // root directory doesn't exist
    MissingToml,     // root exists but instance.toml missing
    CorruptedToml,   // instance.toml exists but can't be parsed
    UnregisteredGame // game_id not recognized by any plugin
  };

  struct ValidatedEntry {
    Entry entry;
    ValidationStatus status;
  };

  // Construction — loads the registry from disk
  InstanceRegistry();
  ~InstanceRegistry();

  // Persistence
  [[nodiscard]] bool load();       // load from instance_registry.toml
  [[nodiscard]] bool save() const; // save to instance_registry.toml

  // Registry path
  [[nodiscard]] static std::filesystem::path registry_path();

  // --- Query ---
  [[nodiscard]] std::vector<Entry> all_entries() const;
  [[nodiscard]] std::vector<ValidatedEntry>
  all_validated() const; // includes validation status
  [[nodiscard]] std::optional<Entry>
  find_by_name(const std::string &name) const;
  [[nodiscard]] std::optional<Entry>
  find_by_root(const std::filesystem::path &root) const;
  [[nodiscard]] std::string active_name() const; // name of active entry

  // --- Mutations ---
  void register_instance(const std::string &name,
                         const std::filesystem::path &root,
                         const std::string &type, const std::string &game_id,
                         const std::string &display_name);
  void unregister_instance(const std::string &name);
  void
  set_active(const std::string &name); // marks one active, all others inactive
  void update_last_used(const std::string &name); // bump timestamp
  void update_root(const std::string &name,
                   const std::filesystem::path &new_root);

  // --- Validation ---
  // Validates all entries. Returns list of entries with problems.
  [[nodiscard]] std::vector<ValidatedEntry> validate_all() const;

  // Validate a single entry
  [[nodiscard]] ValidationStatus validate(const Entry &entry) const;

  // Handle a missing instance — called by UI when validation finds MissingRoot
  struct RepairResult {
    bool repaired = false;
    bool removed = false;
    std::filesystem::path new_root; // only if repaired
  };
  RepairResult repair_missing(const std::string &name);

private:
  std::vector<Entry> entries_;
  std::filesystem::path path_;

  [[nodiscard]] std::string now_iso8601() const;
  [[nodiscard]] std::string sanitize_name(const std::string &name) const;
};

} // namespace engine
