#pragma once

// Installed-pack record for an instance (Workspace-by7k).
//
// Tracks per-mod state for pack-installed mods so incremental updates can
// honor user intent: which mods came from the pack, which the user removed,
// which the user repositioned, what was actually installed, plus the pack
// identity, tree snapshot, applied patches/ini edits, and ini tweak toggles.
//
// Persisted as <instance_root>/installed_pack.json. Qt-free, engine layer.
// Atomic writes reuse engine::profile::safe_write_file.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine {

// Where a tracked mod came from. Manual mods are fully outside pack-diff
// logic; generated mods (tool output captured as a synthetic mod) are fully
// replaced on re-run rather than merged.
enum class PackModOrigin {
  Pack,
  Manual,
  Generated,
};

// Whether the mod is currently present. Removed means the user deleted a
// pack-originated mod: future updates skip it unless explicitly restored.
enum class PackModPresence {
  Installed,
  Removed,
};

// Tree.json position ownership. Diverged means the user manually reordered
// or regrouped the mod: the tree-diff step of future updates leaves it
// alone, but version/source updates still flow. Never self-heals back to
// Conforming; only reset_to_pack_layout() does that.
enum class PackModPlacement {
  Conforming,
  Diverged,
};
struct ResolvedModEntry {
  PackModOrigin origin = PackModOrigin::Pack;
  PackModPresence presence = PackModPresence::Installed;
  PackModPlacement placement = PackModPlacement::Conforming;
  // Pack revision this mod's content was last applied from.
  int64_t last_applied_revision = 0;
  // What was actually resolved/installed (may differ from the pack's pin
  // for updatePolicy:latest mods or integrity-flagged mismatches).
  int64_t actual_file_id = 0;
  std::string actual_version;
  std::string actual_hash;
  // For origin::Generated: the executable id that produced this mod.
  std::string produced_by;
};

// Manages the installed-pack record for a single instance.
// Reads/writes <instance_root>/installed_pack.json using nlohmann/json.
class InstalledPackState {
public:
  // Create state for the given instance root.
  explicit InstalledPackState(std::filesystem::path instance_root);

  // Load state from installed_pack.json. Returns false on I/O/parse error
  // (in-memory state left untouched). A missing file is valid empty state
  // (returns true).
  bool load();

  // Save state to installed_pack.json atomically. Returns false on I/O error.
  bool save() const;

  // Forget everything: pack identity, resolved mods, tree snapshot,
  // applied patches/ini edits, ini tweaks. Used when a pack is uninstalled.
  void clear();

  // -- pack identity --
  [[nodiscard]] bool has_pack() const;
  void set_pack(std::string pack_id, int64_t revision);
  [[nodiscard]] const std::string& pack_id() const;
  [[nodiscard]] int64_t installed_revision() const;

  // -- per-mod state --
  // Ensure a pack-originated mod has an entry (pack/installed/conforming
  // defaults). Never touches an existing entry's presence/placement.
  ResolvedModEntry& ensure_pack_mod(const std::string& mod_id);
  // Record what a pack mod resolved to. Creates the entry if absent;
  // never touches presence/placement (a diverged mod stays diverged).
  void record_pack_version(const std::string& mod_id, int64_t file_id,
                           const std::string& version, const std::string& hash,
                           int64_t revision);
  // Mark a mod as user-added (outside pack-diff logic) or generated
  // (tool output, fully replaced on re-run).
  void mark_manual(const std::string& mod_id);
  void mark_generated(const std::string& mod_id, const std::string& produced_by);
  // User removed a pack mod / explicitly restored it from the pack.
  void mark_removed(const std::string& mod_id);
  void restore_from_pack(const std::string& mod_id);
  // User repositioned a pack mod / explicitly reset it to the pack layout.
  void mark_diverged(const std::string& mod_id);
  void reset_to_pack_layout(const std::string& mod_id);

  [[nodiscard]] const ResolvedModEntry* resolved_mod(const std::string& mod_id) const;
  [[nodiscard]] bool is_tracked(const std::string& mod_id) const;
  // True when the update diff must skip this mod entirely: user-added
  // (manual) or user-removed. Untracked ids are NOT skipped (fresh install).
  [[nodiscard]] bool skip_in_update(const std::string& mod_id) const;
  // True when the tree-diff step owns this mod's position: installed and
  // conforming, and not user-added. Untracked ids return false.
  [[nodiscard]] bool follows_pack_tree(const std::string& mod_id) const;
  [[nodiscard]] const std::unordered_map<std::string, ResolvedModEntry>&
  all_resolved_mods() const;

  // -- tree snapshot (raw tree.json text at apply time, for tree diff) --
  void set_tree_snapshot(std::string snapshot);
  [[nodiscard]] const std::string& tree_snapshot() const;

  // -- applied patches / ini edits (ids, for update diffing) --
  void set_applied_patches(std::vector<std::string> patches);
  [[nodiscard]] const std::vector<std::string>& applied_patches() const;
  void set_applied_ini_edits(std::vector<std::string> edits);
  [[nodiscard]] const std::vector<std::string>& applied_ini_edits() const;

  // -- ini tweaks (tweakId -> enabled; instance owns the toggle) --
  void set_ini_tweak(const std::string& tweak_id, bool enabled);
  [[nodiscard]] std::optional<bool> ini_tweak_enabled(
      const std::string& tweak_id) const;
  [[nodiscard]] const std::unordered_map<std::string, bool>& ini_tweaks() const;

  [[nodiscard]] const std::filesystem::path& instance_root() const;
  [[nodiscard]] std::filesystem::path file_path() const;

private:
  std::filesystem::path instance_root_;
  std::string pack_id_;
  int64_t installed_revision_ = 0;
  std::unordered_map<std::string, ResolvedModEntry> resolved_mods_;
  std::string tree_snapshot_;
  std::vector<std::string> applied_patches_;
  std::vector<std::string> applied_ini_edits_;
  std::unordered_map<std::string, bool> ini_tweaks_;
};

[[nodiscard]] const char* to_string(PackModOrigin origin);
[[nodiscard]] const char* to_string(PackModPresence presence);
[[nodiscard]] const char* to_string(PackModPlacement placement);
// Unknown strings fall back to the pack defaults (Pack/Installed/
// Conforming) so older readers tolerate newer writers.
[[nodiscard]] PackModOrigin pack_origin_from_string(const std::string& s);
[[nodiscard]] PackModPresence pack_presence_from_string(const std::string& s);
[[nodiscard]] PackModPlacement pack_placement_from_string(const std::string& s);

}  // namespace engine
