#pragma once

// Modpack export (write side of .gmmpack). Reads an InstanceSnapshot plus
// each mod's in-folder meta.ini and produces a schema-valid Gmmpack struct
// (and, via create_gmmpack, a .gmmpack archive on disk).
//
// Mirror of unpacker.h: the serialize_* helpers here emit exactly the
// camelCase field names the parse_* functions in unpacker.cpp read.

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/instance/instance_snapshot.h"
#include "engine/gmmpack/types.h"
#include "engine/mod/meta/mod_meta.h"

namespace engine::gmmpack {

// The stages of a pack build, in the order build_gmmpack runs them. A build
// publishes the one it is in so a UI can say what is happening instead of
// guessing; there are kPackStageCount of them, so a bar driven by PackProgress
// gets one part per stage plus the share of the current one.
enum class PackStage {
  Sources,      // resolve every exported mod's source; a bundled mod is hashed here
  Executables,  // resolve sources again, to validate sourceModId references
  Ini,          // parse every INI the exported mods ship
  Patches,      // bsdiff every file two mods ship at the same path
  Tree,         // resolve sources a third time, for the separator/mod layout
  Payload,      // read the bundled bytes into RAM for the archive
  Count,
};
inline constexpr int kPackStageCount = static_cast<int>(PackStage::Count);

// What a running build publishes for a UI to poll. Every counter is its own
// atomic and the name has its own lock, so a reader never sees a half-written
// set and the writer never blocks on the counters. No Qt: the engine stays
// Qt-free, so the counters are polled, not signalled.
struct PackProgress {
  std::atomic<int> stage{static_cast<int>(PackStage::Sources)};
  std::atomic<int> item{0};          // units of the current stage finished
  std::atomic<int> total{0};         // units it will do; 0 = not counted
  std::atomic<int> detail_item{0};   // files finished inside the current item
  std::atomic<int> detail_total{0};  // files the current item has
  mutable std::mutex name_mutex;
  std::string name;  // what is being worked on, e.g. a mod folder or a file

  // 0..1 across the whole build. A stage that reports no unit count sits on its
  // opening boundary rather than inventing motion inside itself, so a build that
  // is stopped partway stops short of 1.
  [[nodiscard]] double fraction() const {
    const int units   = total.load(std::memory_order_relaxed);
    const double done = item.load(std::memory_order_relaxed);
    const double within =
        units > 0 ? std::clamp(done / static_cast<double>(units), 0.0, 1.0) : 0.0;
    return (static_cast<double>(stage.load(std::memory_order_relaxed)) + within) /
           static_cast<double>(kPackStageCount);
  }
};

// Cooperative cancellation for the long pack builders. Building a pack hashes
// every bundled byte and binary-diffs every file two mods ship at the same path,
// so it is unbounded work and cannot run on a GUI thread. A caller that runs a
// build on a worker thread hands in one of these to abandon the build at the
// next file boundary, and - when it wants a progress bar - a shared sink the
// build publishes into. Absent (the default) = never cancelled, no progress.
// std::atomic, not Qt, so the engine stays Qt-free.
struct PackCancel {
  std::atomic<bool> flag{false};
  std::shared_ptr<PackProgress> progress;

  [[nodiscard]] bool cancelled() const { return flag.load(std::memory_order_relaxed); }
  void raise() { flag.store(true, std::memory_order_relaxed); }
  void clear() { flag.store(false, std::memory_order_relaxed); }
};

struct PackOptions {
  std::string author;
  std::string description;
  std::string homepage;
  std::string instructions;
  // Per-mod update policy keyed by mod FOLDER ("latest" or "exact").
  // Absent = "latest". Applied by build_mod_entries after
  // resolve_mod_source; steam_workshop sources always stay "latest", and a
  // source whose download identity cannot be resolved degrades to "latest"
  // rather than emitting a pin it cannot back up.
  std::unordered_map<std::string, std::string> update_policies;
  // Mod FOLDER -> "required" | "recommended" | "optional". Absent = required:
  // a mod nobody labelled is a mod the author deliberately put in the pack.
  std::unordered_map<std::string, std::string> categories;
  // Mod FOLDERS whose own files are bundled into the archive (files/<id>/).
  // Only meaningful for sources with no download identity: a manual or
  // unknown-source mod has nothing else to point at.
  std::unordered_set<std::string> embed_folders;
  // Author-declared mutually exclusive mod groups, shipped as-is.
  std::vector<ChoiceGroup> choice_groups;
  // Where the instance's downloaded archives live. Used to derive the
  // download identity (size + sha256) an exact pin needs, for mods installed
  // before that identity was recorded in their meta.ini.
  std::filesystem::path downloads_dir;
  // Schema directory for the validation create_gmmpack runs on its own output.
  // Empty = referential integrity only (no schemas to validate against).
  std::filesystem::path schema_dir;
};

struct PackResult {
  bool ok = false;
  std::string error;
  std::filesystem::path output_path;
  // Files written under files/<mod-id>/, and the mods carrying them.
  size_t embedded_mod_count  = 0;
  size_t embedded_file_count = 0;
};

// Map a mod's meta.ini to its pack source. Nullopt for manual/unknown
// sources (the pack format has no manual provider - use
// resolve_embedded_source for those). game_id is the GMM game id (nexus
// gameDomain fallback); steam_appid feeds the steam_workshop appId field.
std::optional<ModSource> resolve_mod_source(const ModMeta &meta,
                                            const std::string &game_id,
                                            uint32_t steam_appid = 0);

// Bundle a mod folder's own files: an embedded source listing every file
// under mods_dir/folder with its size and sha256. Nullopt when the folder is
// missing, holds no regular files (nothing to bundle), or `cancel` was raised
// before the last file was hashed.
std::optional<ModSource> resolve_embedded_source(const std::filesystem::path &mods_dir,
                                                 const std::string &folder,
                                                 const PackCancel *cancel = nullptr);

// Can this folder be bundled at all? Cheap directory walk, no hashing - the
// UI asks this once per row.
bool can_embed_folder(const std::filesystem::path &mods_dir, const std::string &folder);

// Source resolution shared by every pack builder: the mod's own source when
// it has one, otherwise its embedded payload when options.embed_folders names
// the folder. Nullopt = the mod cannot be represented in a pack at all.
std::optional<ModSource>
resolve_for_export(const ModMeta &meta, const std::filesystem::path &mods_dir,
                   const std::string &folder, const std::string &game_id,
                   uint32_t steam_appid, const PackOptions &options,
                   const PackCancel *cancel = nullptr);

// The download identity an "exact" pin needs, taken from the mod's own
// recorded install metadata. Empty fields mean "not recorded", which is what
// makes an unresolvable pin degrade to "latest" instead of fabricating one.
struct DownloadIdentity {
  int64_t file_size = 0;
  std::string sha256;
  std::string version;
  std::string file_name;
};
DownloadIdentity read_download_identity(const ModMeta &meta);

// Same, but falls back to hashing the archive in downloads_dir named by the
// mod's recorded installation file. Returns a zero identity when neither
// source can supply one.
DownloadIdentity resolve_download_identity(const ModMeta &meta,
                                           const std::filesystem::path &downloads_dir);

// Folder name -> schema-valid mod id slug. Lowercase alnum runs joined by
// single hyphens; collisions get -2/-3 suffixes. Deterministic for a given
// input set (inputs are sorted before slugging).
std::string mod_slug(const std::string &folder_name);

// Build the display tree from mod-state nesting. Separators are entries
// whose folder name appears as another entry's parent_separator; children
// sort by list_position. ModNode enabled = !hidden && !disabled. Mods with
// no representable source are omitted (they have no mods/<id>.json to point
// at). Resolves every mod's source, so an embedded mod's files are hashed
// here too.
TreeRoot build_tree(const InstanceSnapshot &snapshot,
                    const std::filesystem::path &mods_dir,
                    const PackCancel *cancel = nullptr);

// Same, for an export that bundles embedded mod folders: a manual/unknown
// mod named in options.embed_folders appears in the tree too.
TreeRoot build_tree(const InstanceSnapshot &snapshot,
                    const std::filesystem::path &mods_dir, const PackOptions &options,
                    const PackCancel *cancel = nullptr);

// Manifest with stable pack identity: reuses snapshot.modpack_id when set
// (fresh UUID v4 otherwise), revision = snapshot.modpack_revision + 1,
// schema "1.0.0", info from snapshot + options, current UTC timestamps.
// Also populated from real instance state: loadOrder.pluginHint from the
// default profile's plugin list (with LOOT declared as a required tool when
// there is one), platform.linux.protonVersionPin from the instance's Proton
// runner, one "requires" rule per executable (an exe needs its mod), and
// options.choice_groups. archive.fileHashes is left empty - create_gmmpack
// fills it after serializing every file.
Manifest build_manifest(const InstanceSnapshot &snapshot, const PackOptions &options);

// One ModEntry per snapshot mod with a representable source: its own, or an
// embedded payload when options.embed_folders names it. Order is
// deterministic (list_position, then folder). category/phase come from
// options.categories; options.update_policies (folder -> "latest"|"exact",
// absent = "latest") overrides the resolved source's update_policy -
// steam_workshop sources always stay "latest", and so does any source whose
// download identity cannot be resolved.
std::vector<ModEntry> build_mod_entries(const InstanceSnapshot &snapshot,
                                        const std::filesystem::path &mods_dir,
                                        const PackOptions &options,
                                        const PackCancel *cancel = nullptr);

// Snapshot executables -> pack executables. Entries whose mod does not
// resolve to an exported mod (game-root exes) are skipped: sourceModId must
// pass referential integrity. role is "setup" for a recognized generator
// (Nemesis/FNIS/BodySlide/xEdit/... - see is_setup_executable) and "launcher"
// otherwise, with autoRun/rerunOnModsetChange set on the former.
std::vector<ExecutableEntry> build_executables(const InstanceSnapshot &snapshot,
                                               const std::filesystem::path &mods_dir,
                                               const PackCancel *cancel = nullptr);

// Same, for an export that bundles embedded manual mods: an executable owned
// by a bundled manual mod is kept (its mod id is in the pack).
std::vector<ExecutableEntry> build_executables(const InstanceSnapshot &snapshot,
                                               const std::filesystem::path &mods_dir,
                                               const PackOptions &options,
                                               const PackCancel *cancel = nullptr);

// True for an executable GMM recognizes as a one-shot setup generator
// (reads the deployed mod set and writes output), false for a launcher.
bool is_setup_executable(const std::string &relative_path);

// One ini/<targetFile>.json entry per INI file shipped by the exported mods,
// one tweak per (mod, file) carrying that file's settings. Empty when no
// exported mod ships an INI file, or when `cancel` was raised first.
std::vector<IniEntry> build_ini_entries(const std::filesystem::path &mods_dir,
                                        const std::vector<ModEntry> &mods,
                                        const PackCancel *cancel = nullptr);

// One bsdiff patch per file that 2+ exported mods ship at the same relative
// path: the higher-priority mod's copy is the base, each lower-priority mod's
// copy becomes a patch against it (this is the pack format's consent-gated
// alternative to silent file-priority conflict resolution). Files above
// ~8 MB are skipped - a base64 diff of a game-sized asset is not something a
// pack should carry. Empty when no exported mods collide, or when `cancel` was
// raised first.
std::vector<PatchEntry> build_patches(const InstanceSnapshot &snapshot,
                                      const std::filesystem::path &mods_dir,
                                      const std::vector<ModEntry> &mods,
                                      const PackCancel *cancel = nullptr);

// JSON serializers (reverse of unpacker.cpp parse_*).
nlohmann::json serialize_manifest(const Manifest &m);
nlohmann::json serialize_mod_source(const ModSource &source);
nlohmann::json serialize_mod_entry(const ModEntry &m);
nlohmann::json serialize_executable_entry(const ExecutableEntry &e);
nlohmann::json serialize_ini_entry(const IniEntry &entry);
nlohmann::json serialize_patch_entry(const PatchEntry &p);
// Archive path of a patch entry: patches/<mod-id>.json for a single patch,
// patches/<mod-id>-<N>.json for chain step N.
std::string patch_archive_path(const PatchEntry &p);

// Assemble the full pack in memory. Reads every exported mod's folder several
// times over (once per stage that resolves sources, once for ini/, once for
// patches/, once to load the bundled payload), so it is as slow as the mod
// folder it is given - never call it on a GUI thread. `cancel` abandons it at
// the next file boundary and receives the progress the build publishes; a
// cancelled result is incomplete and must be discarded.
//
// Measured on a 199-mod / 5.6 GB Skyrim SE instance: every stage except
// Patches together take about 11 s, while Patches - one bsdiff per file two
// mods ship at the same path, 1,061 of them - took over 40 minutes. On that
// instance Patches is ~99% of the build, so a caller that only needs the
// layout should not be paying for it.
Gmmpack build_gmmpack(const InstanceSnapshot &snapshot,
                      const std::filesystem::path &mods_dir, const PackOptions &options,
                      const PackCancel *cancel = nullptr);

// Serialize + write a .gmmpack (zip) archive: manifest.json, tree.json,
// mods/*.json, executables/*.json, files/** for embedded mods,
// patches/*.json, ini/*.json, instructions.md when present. fileHashes are
// computed over the serialized payloads and baked into the manifest before
// writing, so the archive passes verify_archive_integrity.
//
// Everything the packer just built is validated before the archive is
// written: validate_schemas (when options.schema_dir is set) and
// check_referential_integrity always. A failure sets result.error and writes
// no file, so the exporter cannot report success on a pack its own importer
// would refuse.
//
// Long-running for the same reason build_gmmpack is: call it on a worker
// thread. Deliberately NOT cancellable - abandoning it mid-write would leave a
// truncated archive at output_path, which is worse than letting it finish.
PackResult create_gmmpack(const InstanceSnapshot &snapshot,
                          const std::filesystem::path &mods_dir,
                          const PackOptions &options,
                          const std::filesystem::path &output_path);

}  // namespace engine::gmmpack
