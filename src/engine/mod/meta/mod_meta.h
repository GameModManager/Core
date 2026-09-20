#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine {

// Section-based metadata for a single mod.
// Format: [General] + [{provider}] sections + [GameModManager].
// Provider sections are arbitrary - any game/plugin can write its own.
class ModMeta {
public:
  std::string get(const std::string& section, const std::string& key) const;
  void set(const std::string& section, const std::string& key,
           const std::string& value);

  bool has_section(const std::string& section) const;
  std::vector<std::string> sections() const;
  std::vector<std::string> keys(const std::string& section) const;

  // Serialize/parse INI format
  std::string serialize() const;
  bool parse(const std::string& content);

  // --- Factories ---

  // Import from an MO2 meta.ini found inside a mod folder.
  // Detects repository=Nexus → moves nexus fields to [Nexusmods],
  // copies [installedFiles] as subkeys under [Nexusmods].
  static ModMeta from_mo2_import(const std::string& content,
                                 const std::string& folder_name);

  // Create fresh meta for a newly added mod (no MO2 history).
  static ModMeta from_default(const std::string& folder_name,
                              const std::string& source_type,
                              const std::string& source_id,
                              const std::string& installation_file = {},
                              const std::string& version           = {});

  // --- Convenience accessors ---

  [[nodiscard]] std::string folder() const;
  [[nodiscard]] std::string source_type() const;
  [[nodiscard]] std::string source_id() const;
  // Source page URL from the per-source section (e.g. [LoversLab] page_url).
  // Empty for sources that reconstruct their link from source_id instead.
  [[nodiscard]] std::string source_page_url() const;
  [[nodiscard]] std::string separator_id() const;
  void set_separator_id(const std::string& id);
  [[nodiscard]] std::string version() const;
  [[nodiscard]] int priority() const;
  void set_priority(int p);
  [[nodiscard]] bool imported_from_mo2() const;

  // --- Per-mod UI state (in-folder meta.ini) ---
  // Tree-view collapse state and visual-nesting parent link. These live in
  // the mod's own meta.ini [GameModManager] section
  // ({instance_root}/mods/{folder_name}/meta.ini, MO2-compatible).
  // folded is explicit true/false; parent_id is absent for
  // top-level rows (unset() clears it back to "absent").
  [[nodiscard]] bool folded() const;
  void set_folded(bool folded_state);
  [[nodiscard]] std::string parent_id() const;
  void set_parent_id(const std::string& id);

  // --- Collection tracking (in-folder meta.ini, Workspace-5wmu) ---
  // Source-agnostic collection membership persisted per mod in
  // [GameModManager]: owning collection id, revision at install time, and
  // the per-mod-in-collection flag. Absent keys = untracked standalone mod.
  [[nodiscard]] std::string collection_id() const;
  void set_collection_id(const std::string& id);
  [[nodiscard]] int64_t collection_revision() const;
  void set_collection_revision(int64_t revision);
  [[nodiscard]] bool in_collection() const;
  void set_in_collection(bool tracked);
  void clear_collection();

  // --- Mirror/backup tracking (Workspace-0pi5) ---
  // External mods (e.g. Steam Workshop) can vanish without user consent.
  // Mirroring copies the source folder into instance/mods/ as a backup.
  // The [Mirror] section records the backup's source: sourcePath is the
  // absolute path of the original mod folder, sourceTimestamp is that
  // folder's mtime at mirror time. Re-mirroring compares the live source
  // mtime against sourceTimestamp and refreshes the backup on mismatch.
  // The section lives in BOTH the source meta and the backup meta: the
  // source copy drives the mirrored badge while the source is present,
  // the backup copy drives source-missing detection and the prune skip
  // after the source is gone.
  [[nodiscard]] bool is_mirrored() const;
  [[nodiscard]] std::string mirror_source_path() const;
  [[nodiscard]] int64_t mirror_source_timestamp() const;
  void set_mirror(const std::string& source_path, int64_t source_mtime);
  void clear_mirror();

  // Remove a single key (and leave the section in place). Used to clear
  // parent_id so a top-level row serializes without the key.
  void unset(const std::string& section, const std::string& key);

  // --- File I/O ---
  // Load/save meta file at {mods_dir}/{folder_name}/meta.ini
  // (MO2-compatible in-folder location). load() falls back to the legacy
  // sidecar {mods_dir}/../meta/{folder_name}.ini for one release so
  // un-migrated instances keep working; save() always writes in-folder.
  static ModMeta load(const std::filesystem::path& mods_dir,
                      const std::string& folder_name);
  bool save(const std::filesystem::path& mods_dir,
            const std::string& folder_name) const;

  // Load/save meta at an explicit .ini path (e.g. a mod's own meta.ini
  // inside the mod folder, or a legacy sidecar path).
  static ModMeta load_file(const std::filesystem::path& ini_file);
  bool save_file(const std::filesystem::path& ini_file) const;

  // Check if a meta file already exists (in-folder or legacy sidecar).
  static bool exists(const std::filesystem::path& mods_dir,
                     const std::string& folder_name);

  // --- MO2 detection ---
  static bool has_mo2_meta(const std::filesystem::path& mod_folder);
  static ModMeta import_mo2(const std::filesystem::path& mod_folder,
                            const std::string& folder_name);

  // --- Game-visible metadata files ---
  // Write the game's metadata file into a mod folder so ModScanner
  // recognizes the mod. MO2-style games get a meta.ini ([General] with
  // modid/version/newestVersion/category/installationFile - the same file
  // MO2's installers write); games that registered the metadata_file hook
  // (Isaac) get their XML metadata file. No-op if the file already exists.
  // Returns true on success (or if it already existed).
  //
  // `nexus_mod_id` is the Nexus mod id for this mod, when the mod was
  // installed from Nexus. It is ONLY written into the MO2-style meta.ini
  // [General] modid= field - the field MO2 reads to look up the mod on
  // Nexus. For mods NOT from Nexus (Steam Workshop, LoversLab, manual
  // archives) callers MUST pass an empty string here: writing a
  // LoversLab file id or Steam workshop id as "modid" makes MO2 / any
  // reader interpret it as a Nexus id and silently mis-attribute the
  // mod's provenance. For non-Nexus providers this function writes
  // modid=0, the "no Nexus id" sentinel MO2 understands.
  static bool write_game_metadata(const std::filesystem::path& mod_dir,
                                  const std::string& metadata_file,
                                  const std::string& display_name,
                                  const std::string& version,
                                  const std::string& nexus_mod_id      = {},
                                  const std::string& installation_file = {});

  // --- Versioning ---
  // Increment this whenever the meta format changes (new sections/keys added).
  // Existing meta files with an older version get upgraded on next load.
  static constexpr int CURRENT_META_VERSION = 1;

  // Returns meta_version from [GameModManager], or 0 if absent (pre-versioning).
  [[nodiscard]] int meta_version() const;
  void set_meta_version(int v);

private:
  // Ordered sections for deterministic serialization.
  // Section order: General → (provider sections, insertion order) → GameModManager
  std::vector<std::pair<std::string, std::unordered_map<std::string, std::string>>>
      sections_;
};

}  // namespace engine
