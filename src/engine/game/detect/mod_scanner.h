#pragma once

#include "engine/mod/model/mod.h"
#include <filesystem>
#include <string>
#include <vector>

namespace engine {

class GameKnowledge;

struct ScannedMod {
  std::string folder_name;   // directory name on disk
  std::string display_name;  // from metadata, normalized (no priority prefix)
  std::string raw_name;      // from metadata, as-is (with priority prefix)
  std::string version;       // from metadata
  std::string
      separator_color;  // hex color for separators (e.g. "#888888"), empty for mods
  ModType type = ModType::Regular;
  int priority = -1;  // extracted from name prefix via priority_prefix_re, -1 = none
  int64_t workshop_id =
      0;  // extracted from folder name via workshop_id_pattern, 0 = none
  // Folder birth time (statx btime, MO2 COL_INSTALLTIME semantics; falls
  // back to the folder's last-write time). 0 = unavailable/not a real folder.
  int64_t install_time = 0;      // epoch seconds
  int64_t changed_time = 0;      // folder last-write time, epoch seconds
  bool enabled         = true;   // false if disable sentinel exists
  bool is_separator    = false;  // true if folder ends with separator_suffix
  bool is_overwrite    = false;  // true for the special Overwrite entry
  bool is_game_native  = false;  // true for game-provided plugins (e.g. vanilla ESMs)
  bool is_fomod =
      false;  // true if meta.ini carries a [fomod] section with saved choices
  bool root_override = false;  // true if the mod deploys to the game root (meta.ini
                               // [General] rootOverride)
  // No recognized metadata file in the folder (MO2 lists every folder in
  // Mods/; the manager warns when it wasn't the one that installed it).
  bool no_metadata = false;
  // Content-validity check failed (MO2's FLAG_INVALID "No valid game data"):
  // the folder holds no recognized game data per the per-game allow-lists.
  bool invalid_data = false;
  // MO2's validated marker ([General] validated=true in the folder's
  // meta.ini, the file markValidated writes). Suppresses the flags above.
  bool validated = false;
  // Folder contains files hidden via .gmmhidden/.mohidden suffix.
  bool has_hidden_files = false;
  // No real files outside meta.ini/metadata.xml (empty mod shell).
  bool is_empty = false;
  // Category IDs auto-assigned from Steam Workshop tags via the
  // workshop_tag_categories hook. Empty when no mapping is available.
  std::vector<int> category_ids;
  // Mirror/backup tracking (Workspace-0pi5): the folder's meta.ini carries
  // a [Mirror] section. mirror_source_path is the original external folder
  // (empty when the section has no sourcePath); mirror_source_missing is
  // true when that path is no longer on disk (deleted by Steam etc.) and
  // the scanned folder is the surviving backup copy.
  bool is_mirrored           = false;
  bool mirror_source_missing = false;
  std::string mirror_source_path;
  // The directory where this mod's actual game content lives. For most
  // mods this is the same as the scanned mods_dir. For games with an
  // external game_mods_dir hook (Isaac), external mods' content lives
  // in game_dir/mods/ while instance/mods/ only has a meta.ini stub.
  // Used by path resolution for mod info, file open, and file manager.
  std::filesystem::path content_dir;
};

// Generic mod scanner - reads ALL game-specific config from GameKnowledge.
// No hardcoded file formats, tag names, or folder conventions.
// Each game plugin tells the engine how to discover and parse its mods.
class ModScanner {
public:
  // Scan a game's mods directory using settings from GameKnowledge.
  // game_id is used to look up hooks in knowledge.
  // Mods-dir resolution goes through resolve_game_mods_dir: the per-instance
  // "game_mods_dir" override (override_mods_dir), then the plugin-declared
  // "game_mods_dir" hook, then game_install_dir/mod_scan_subpath. A set
  // game_mods_dir IS the mods folder - nothing is appended to it.
  // Workspace-s3hn: mods_subpath and the game_install_dir fallbacks are
  // intentionally NOT in this chain. mods_subpath is a deploy target,
  // never a scan source; falling back to it (or to game_install_dir
  // itself) would walk vanilla game content (Data/, SKSE, Scripts,
  // Meshes, Source, ...) and synthesize it as ScannedMod rows. When the
  // resolution returns an empty path, the scan returns empty and the
  // caller is expected to fall back to the instance mods dir.
  [[nodiscard]] static std::vector<ScannedMod>
  scan(const GameKnowledge &knowledge, const std::string &game_id,
       const std::filesystem::path &game_install_dir,
       const std::vector<std::filesystem::path> &ignore_symlink_targets = {},
       const std::filesystem::path &override_mods_dir                   = {});

  // Scan a specific mods directory directly (bypasses mods_subpath resolution).
  [[nodiscard]] static std::vector<ScannedMod>
  scan_dir(const GameKnowledge &knowledge, const std::string &game_id,
           const std::filesystem::path &mods_dir,
           const std::vector<std::filesystem::path> &ignore_symlink_targets = {});

  // Scan a single mod folder (installed_missing_stages: the install pipeline
  // produces one folder at a time, so the UI can add just that row instead of
  // rescanning the whole mods dir). Returns empty when the folder holds no
  // recognized mod.
  [[nodiscard]] static std::vector<ScannedMod>
  scan_folder(const GameKnowledge &knowledge, const std::string &game_id,
              const std::filesystem::path &mods_dir, const std::string &folder_name,
              const std::vector<std::filesystem::path> &ignore_symlink_targets = {});

  // Create the disable sentinel file for a mod.
  [[nodiscard]] static bool disable_mod(const GameKnowledge &knowledge,
                                        const std::string &game_id,
                                        const std::filesystem::path &mod_folder);

  // Remove the disable sentinel file to enable a mod.
  [[nodiscard]] static bool enable_mod(const GameKnowledge &knowledge,
                                       const std::string &game_id,
                                       const std::filesystem::path &mod_folder);

  // Set the priority of a mod by rewriting its metadata.
  [[nodiscard]] static bool set_priority(const GameKnowledge &knowledge,
                                         const std::string &game_id,
                                         const std::filesystem::path &mod_folder,
                                         int priority);

  // MO2's "Ignore missing data": persist [General] validated=true in the
  // folder's meta.ini (creating it if absent) so the invalid/no-metadata
  // flags stay cleared on rescan. Returns false on write failure.
  [[nodiscard]] static bool mark_validated(const std::filesystem::path &mod_folder);

  // Delete ghost mod stubs left behind when an external source (e.g. a
  // Steam Workshop unsubscribe) removes a mod's real files but leaves the
  // manager-written meta.ini behind, so the folder rescans forever as
  // is_empty (Workspace-5jk3).
  //
  // A folder is deleted ONLY when all of these hold:
  //   - its scan result is is_empty, and it is not a separator, the
  //     Overwrite entry, a MERGED pseudo-row, or a game-native row,
  //   - the folder physically exists under mods_dir (external-only and
  //     synthesized rows have no instance folder and are never touched),
  //   - the folder is not a mirror backup ([Mirror] in its meta.ini
  //     exempts it unconditionally - the backup is the user's fallback
  //     when the external source is gone),
  //   - proof the manager owned the folder: a ModStateTracker entry for
  //     it (installed or snapshotted while it still had content), OR -
  //     for pre-seeding legacy ghosts - a [GameModManager] section in
  //     its meta.ini without imported_from_mo2=true (a user-created
  //     shell never carries that section, and MO2-import enrichment
  //     stamps the exemption) COMBINED with a caller-supplied
  //     external_mods_dir whose <folder> is gone (Isaac: Steam
  //     unsubscribed). Callers without an external source dir pass
  //     empty and keep tracker-only behavior; the external dir itself
  //     is never modified.
  // Before pruning, currently-healthy scanned mods are seeded into the
  // tracker (nothing in production called record_install, so without
  // seeding the gate above would stay a permanent no-op); the tracker
  // is saved when seeding or pruning changed it.
  // An empty instance_root (portable mode) or an unreadable tracker
  // disables pruning entirely. Returns the pruned folder names so the
  // caller can drop them from its own result list.
  [[nodiscard]] static std::vector<std::string>
  prune_orphaned_empty_mods(const std::vector<ScannedMod> &scanned,
                            const std::filesystem::path &mods_dir,
                            const std::filesystem::path &instance_root,
                            const std::filesystem::path &external_mods_dir = {});
};

}  // namespace engine
