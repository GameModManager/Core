#pragma once

// Per-mod/separator state tracker for an instance. Stores:
// - install order + timestamp (Workspace-b1sb)
// - list position, nesting depth, parent separator, separator color/collapsed (Workspace-1spd)
// - hidden/disabled state per mod + hidden files per mod (Workspace-ofzq)
// - deployAtRoot flag per mod (Workspace-0tsl)
//
// Persisted as <instance_root>/mod_state.json. Qt-free, engine layer only.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine {

// Per-mod (or separator) state entry. Separators are tracked identically to
// mods - they share the same folder-on-disk semantics.
struct ModTrackingEntry {
  // -- install order (b1sb) --
  uint32_t install_order = 0;   // monotonically increasing per install
  int64_t installed_at = 0;     // epoch seconds, last install/update time

  // -- position & hierarchy (1spd) --
  int32_t list_position = -1;   // flat list position (priority), -1 = unset
  std::string parent_separator; // folder_name of enclosing separator, empty = top-level
  int32_t depth = 0;            // nesting depth (0 = top-level)
  std::string separator_color;  // hex color for separators, empty for mods
  bool collapsed = false;       // separator collapsed state in UI

  // -- hidden / disabled (ofzq) --
  bool hidden = false;          // mod is hidden from deployment
  bool disabled = false;        // mod is disabled (sentinel present on disk)
  std::vector<std::string> hidden_files; // relative paths of hidden files within the mod

  // -- deploy at root (0tsl) --
  bool deploy_at_root = false;  // deploy files to game root instead of mods dir
};

// Manages per-mod tracking state for a single instance.
// Reads/writes <instance_root>/mod_state.json using nlohmann/json.
class ModStateTracker {
public:
  // Create a tracker for the given instance root.
  explicit ModStateTracker(std::filesystem::path instance_root);

  // Load state from mod_state.json. Returns false on I/O/parse error.
  // A missing file is treated as empty state (returns true).
  bool load();

  // Save state to mod_state.json atomically. Returns false on I/O error.
  bool save() const;

  // Get or create a mutable entry for a mod folder name.
  [[nodiscard]] ModTrackingEntry& entry(const std::string& folder_name);
  [[nodiscard]] const ModTrackingEntry* entry(const std::string& folder_name) const;

  // Remove a mod's entry entirely.
  void remove(const std::string& folder_name);

  // Record an install event: bumps install_order and stamps installed_at.
  void record_install(const std::string& folder_name);

  // Set list position for a mod/separator.
  void set_position(const std::string& folder_name, int32_t position);

  // Set nesting info for a mod.
  void set_nesting(const std::string& folder_name,
                   const std::string& parent_separator, int32_t depth);

  // Set separator visual properties.
  void set_separator_visual(const std::string& folder_name,
                            const std::string& color, bool collapsed);

  // Toggle hidden/disabled state.
  void set_hidden(const std::string& folder_name, bool hidden);
  void set_disabled(const std::string& folder_name, bool disabled);
  void set_hidden_files(const std::string& folder_name,
                        std::vector<std::string> files);

  // Set deployAtRoot flag.
  void set_deploy_at_root(const std::string& folder_name, bool at_root);

  // Next install order value (one past the current max).
  [[nodiscard]] uint32_t next_install_order() const;

  // Direct access to all entries (for modpack export/import).
  [[nodiscard]] const std::unordered_map<std::string, ModTrackingEntry>&
  all_entries() const;
  void set_all_entries(std::unordered_map<std::string, ModTrackingEntry> entries);

  [[nodiscard]] const std::filesystem::path& instance_root() const;

private:
  std::filesystem::path instance_root_;
  std::unordered_map<std::string, ModTrackingEntry> entries_;
};

} // namespace engine
