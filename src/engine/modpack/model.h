#pragma once

// The modpack install model: everything a validated .gmmpack hands to install.
//
// This is domain core, not format. The structs below describe a pack as the
// installer consumes it - manifest identity, mod entries and their sources,
// executables, patches, tree placement, validation diagnostics - and they are
// read as install INPUT by engine/install (fresh_install, append_install) and
// by engine/modpack/incremental_update. The gmmpack format layer parses and
// writes these shapes; it does not own them.
//
// Engine layer - Qt-free.

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace engine::gmmpack {

// ---------------------------------------------------------------------------
// INI types (ini/<targetFile>.json)
//
// The one part of the model that stays format-scoped: these mirror the tweak
// shape in schemas/ini.schema.json and exist only as parser I/O. status is the
// raw "required"/"recommended" string; the typed enum lives in
// engine/modpack/ini_edits.h (see to_edit_file() in gmmpack/ini_edit_parser.h).
// ---------------------------------------------------------------------------

struct IniTweak {
  std::string id;             // stable slug; key for diffing/state/retract
  std::string name;           // human-readable label
  std::string status;         // "required", "recommended"
  bool enabled = true;        // author default
  std::string content;        // plain INI text
  std::string source_mod_id;  // empty = null (pack-author tweak)
  bool has_source_mod_id = false;
};

struct IniEntry {
  std::string target_file;
  std::vector<IniTweak> tweaks;
};

}  // namespace engine::gmmpack

namespace engine::modpack {

// ---------------------------------------------------------------------------
// Manifest types (manifest.json)
// ---------------------------------------------------------------------------

struct ManifestInfo {
  std::string name;
  std::string author;
  std::string description;
  std::string gmm_game_id;
  std::string homepage;
  std::string created_at;
  std::string updated_at;
};

struct ManifestTool {
  std::string id;
  std::string name;
  std::string homepage;
};

struct PlatformPrefixFile {
  std::string path;
  std::string source_mod_id;
};

struct PlatformOverride {
  std::string proton_version_pin;
  std::optional<bool> steam_overlay;
  std::string launch_options;
  std::vector<PlatformPrefixFile> prefix_files;
};

struct PlatformBlock {
  std::optional<PlatformOverride> linux_plat;
  std::optional<PlatformOverride> macos;
  std::optional<PlatformOverride> windows;
};

struct ManifestRule {
  std::string type;  // "before", "after", "requires", "conflicts"
  std::string from;
  std::string to;
  std::string note;
};

struct ManifestLoadOrder {
  std::vector<std::string> plugin_hint;
};

struct ChoiceGroup {
  std::string id;
  std::string name;
  std::string mode;  // "exactly-one", "at-most-one"
  std::vector<std::string> member_mod_ids;
};

struct ManifestArchive {
  std::unordered_map<std::string, std::string> file_hashes;
};

// Per-instance settings carried in manifest.json ("instanceSettings").
// v1 ships a SINGLE settings block built from the default profile
// (profiles[0]) + the snapshot deploy_strategy, documented as
// "default-profile settings". A per-profile array is a later extension.
struct InstanceSettings {
  bool local_saves               = false;
  bool local_settings            = false;
  bool auto_archive_invalidation = false;
  std::string deploy_strategy;
};

struct Manifest {
  std::string gmmpack_schema;
  std::string id;
  int revision = 0;
  ManifestInfo info;
  std::vector<ManifestTool> tools;
  PlatformBlock platform;
  std::vector<ManifestRule> rules;
  ManifestLoadOrder load_order;
  std::vector<ChoiceGroup> choice_groups;
  ManifestArchive archive;
  InstanceSettings instance_settings;
};

// ---------------------------------------------------------------------------
// Mod source types (mods/<id>.json)
// ---------------------------------------------------------------------------

struct ModSourceNexus {
  std::string provider = "nexus";
  std::string resolution;
  std::string game_domain;
  int64_t mod_id = 0;
  std::optional<int64_t> file_id;
  std::optional<std::string> version;
  std::optional<std::string> file_name;
  std::optional<int64_t> file_size;
  std::optional<std::string> sha256;
  // Nexus publishes an md5 of the archive and never a sha256, so it travels in
  // its own field. sha256 keeps its meaning: a 64-hex digest, required by an
  // exact pin. md5 is informational and never satisfies that requirement.
  std::optional<std::string> md5;
  std::string update_policy = "exact";
};

struct ModSourceLoversLab {
  std::string provider   = "loverslab";
  std::string resolution = "browser";
  std::variant<int64_t, std::string> mod_id;
  std::string section_slug;
  std::optional<std::string> version;
  std::optional<std::string> file_name;
  std::optional<std::string> sha256;
  std::string update_policy = "exact";
};

struct ModSourceModPub {
  std::string provider = "modpub";
  std::string resolution;
  std::variant<int64_t, std::string> mod_id;
  std::optional<std::string> version;
  std::optional<std::string> file_name;
  std::optional<std::string> sha256;
  std::string update_policy = "exact";
};

struct ModSourceSteamWorkshop {
  std::string provider     = "steam_workshop";
  std::string resolution   = "client-subscription";
  int64_t app_id           = 0;
  int64_t workshop_item_id = 0;
  std::optional<std::string> version;
  std::string update_policy = "latest";
};

struct ModSourceDirect {
  std::string provider = "direct";
  std::string resolution;
  std::string url;
  std::optional<std::string> version;
  std::optional<std::string> file_name;
  std::optional<std::string> sha256;
  std::string update_policy = "exact";
};

// A mod whose own files ship inside the archive, under files/<root>/. The one
// source variant that is not a pointer to somewhere else: a manual mod (or one
// whose provider GMM cannot name) carries no download identity, so exporting it
// means carrying the bytes. `files` lists every payload file with the size and
// sha256 import verifies before writing it to the instance's mods folder.
struct ModSourceEmbedded {
  std::string provider   = "embedded";
  std::string resolution = "archive";  // resolved from the archive itself

  struct File {
    std::string path;  // relative to root
    int64_t size = 0;
    std::string sha256;
  };
  std::string root;  // directory inside files/, normally the mod id
  std::vector<File> files;
  // Always "latest": the content is whatever THIS revision of the pack
  // carries, so there is no separate version identity to pin. The per-file
  // sha256 list above is the integrity pin, verified at install time.
  std::string update_policy = "latest";
};

using ModSource =
    std::variant<ModSourceNexus, ModSourceLoversLab, ModSourceModPub,
                 ModSourceSteamWorkshop, ModSourceDirect, ModSourceEmbedded>;

enum class ModCategory {
  Required,
  Optional,
  Recommended,
};

struct InstallerChoices {
  std::string type;
  std::unordered_map<std::string, std::vector<std::string>> selections;
};

struct ModEntry {
  std::string id;
  std::string name;
  int phase            = 0;
  ModCategory category = ModCategory::Optional;
  ModSource source;
  std::optional<InstallerChoices> installer_choices;
};

// ---------------------------------------------------------------------------
// Executable types (executables/<id>.json)
// ---------------------------------------------------------------------------

struct ExecPlatformOverride {
  std::unordered_map<std::string, std::string> env_vars;
  std::string launch_options;
  std::string proton_version_pin;
  std::optional<bool> steam_overlay;
};

struct ExecPlatform {
  std::optional<ExecPlatformOverride> linux_plat;
  std::optional<ExecPlatformOverride> macos;
  std::optional<ExecPlatformOverride> windows;
};

struct ExecOutput {
  std::string path;
  std::string arg_name;
  std::string capture;  // "syntheticMod", "inPlace"
  std::string synthetic_mod_id;
};

struct ExecutableEntry {
  std::string id;
  std::string source_mod_id;
  std::string relative_path;
  std::vector<std::string> arguments;
  std::unordered_map<std::string, std::string> env_vars;
  std::string working_dir;
  std::string role;  // "setup", "launcher"
  bool auto_run                    = false;
  bool rerun_on_modset_change      = false;
  bool requires_virtual_fs_visible = false;
  std::optional<ExecOutput> output;
  ExecPlatform platform;
};

// ---------------------------------------------------------------------------
// Patch types (patches/<id>[-N].json)
// ---------------------------------------------------------------------------

struct PatchEntry {
  std::string mod_id;
  std::optional<int> sequence;
  std::string target_path;
  std::string base_file_sha256;
  std::string algorithm;  // "bsdiff"
  std::string payload_base64;
  std::string archive_path;  // original path in the archive (e.g. "patches/skyui.json")
};

// A sorted chain of patches for one mod, applied in ascending sequence order.
struct PatchChain {
  std::string mod_id;
  std::vector<PatchEntry> patches;  // sorted by sequence ascending
};

// ---------------------------------------------------------------------------
// Tree types (tree.json)
// ---------------------------------------------------------------------------

struct TreeNode;

struct SeparatorNode {
  std::string name;
  bool collapsed = false;
  std::string color;  // hex color, empty = none
  std::vector<TreeNode> children;
};

struct ModNode {
  std::string id;
  bool enabled = true;
};

struct TreeNode {
  std::variant<SeparatorNode, ModNode> data;
};

struct TreeRoot {
  std::vector<TreeNode> nodes;
};

// ---------------------------------------------------------------------------
// Validation diagnostics
// ---------------------------------------------------------------------------

struct Diagnostic {
  enum class Severity { Error, Warning };
  Severity severity = Severity::Error;
  std::string path;
  std::string message;
};

using Diagnostics = std::vector<Diagnostic>;

// ---------------------------------------------------------------------------
// Archive contents (raw, pre-validation)
// ---------------------------------------------------------------------------

struct ArchiveFile {
  std::string path;
  std::string content;
};

struct ArchiveContents {
  std::vector<ArchiveFile> files;
  std::unordered_map<std::string, size_t> path_index;
};

// ---------------------------------------------------------------------------
// Validated gmmpack
// ---------------------------------------------------------------------------

struct Gmmpack {
  Manifest manifest;
  std::vector<ModEntry> mods;
  std::vector<ExecutableEntry> executables;
  std::vector<PatchEntry> patches;
  std::vector<gmmpack::IniEntry> ini_edits;
  TreeRoot tree;
  std::optional<std::string> instructions;
  // files/** entries, kept so an embedded mod can be written out without
  // re-reading the archive (see extract_embedded_mod).
  std::vector<ArchiveFile> payload;
};

// ---------------------------------------------------------------------------
// Semver validation
// ---------------------------------------------------------------------------

struct SemverParse {
  int major = 0;
  int minor = 0;
  int patch = 0;
};

// Parse and validate a semver string "MAJOR.MINOR.PATCH".
// Returns nullopt on any format error.  Does NOT reject major != 1 - callers
// decide that policy (the schema doc says "any other major is a hard refusal").
inline std::optional<SemverParse> parse_semver(const std::string &v) {
  SemverParse sp;
  size_t pos = 0;

  auto parse_int = [&](int &out) -> bool {
    if (pos >= v.size() || !std::isdigit(static_cast<unsigned char>(v[pos])))
      return false;
    int val = 0;
    while (pos < v.size() && std::isdigit(static_cast<unsigned char>(v[pos]))) {
      val = val * 10 + (v[pos] - '0');
      ++pos;
    }
    out = val;
    return true;
  };

  if (!parse_int(sp.major))
    return std::nullopt;
  if (pos >= v.size() || v[pos] != '.')
    return std::nullopt;
  ++pos;
  if (!parse_int(sp.minor))
    return std::nullopt;
  if (pos >= v.size() || v[pos] != '.')
    return std::nullopt;
  ++pos;
  if (!parse_int(sp.patch))
    return std::nullopt;
  if (pos != v.size())
    return std::nullopt;  // trailing junk

  return sp;
}

// ---------------------------------------------------------------------------
// Install-side helpers over the model
// ---------------------------------------------------------------------------

// Mod lookup by id. Null when the pack has no such mod.
const ModEntry *find_mod(const Gmmpack &pack, const std::string &mod_id);

// Extract mod_id and optional sequence from a patch archive path.
// Valid patterns: "patches/<mod_id>.json" (single) or "patches/<mod_id>-<N>.json"
// (chain). Returns nullopt if the path doesn't match the expected pattern.
std::optional<std::pair<std::string, int>>
parse_patch_filename(const std::string &path);

// Group patches by mod_id, sort each group by sequence ascending,
// and validate contiguity (1, 2, 3...). Single patches (no sequence)
// become a chain of one. A mod with both single and chained patches is
// an error. Chains are always returned alongside diagnostics - callers
// must refuse to apply when diagnostics contain errors.
std::pair<std::vector<PatchChain>, Diagnostics>
build_patch_chains(const std::vector<PatchEntry> &patches);

}  // namespace engine::modpack

// ---------------------------------------------------------------------------
// Format-namespace aliases
//
// The model moved here out of engine/modpack/gmmpack/types.h, but engine::gmmpack
// was its address for long enough that every consumer, including the parked
// engine/install headers, still spells these types that way. Re-export them so
// both spellings name the same type and nothing outside the format layer has to
// be rewritten to follow the move.
// ---------------------------------------------------------------------------

namespace engine::gmmpack {

using engine::modpack::ArchiveContents;
using engine::modpack::ArchiveFile;
using engine::modpack::ChoiceGroup;
using engine::modpack::Diagnostic;
using engine::modpack::Diagnostics;
using engine::modpack::ExecOutput;
using engine::modpack::ExecPlatform;
using engine::modpack::ExecPlatformOverride;
using engine::modpack::ExecutableEntry;
using engine::modpack::Gmmpack;
using engine::modpack::InstallerChoices;
using engine::modpack::InstanceSettings;
using engine::modpack::Manifest;
using engine::modpack::ManifestArchive;
using engine::modpack::ManifestInfo;
using engine::modpack::ManifestLoadOrder;
using engine::modpack::ManifestRule;
using engine::modpack::ManifestTool;
using engine::modpack::ModCategory;
using engine::modpack::ModEntry;
using engine::modpack::ModNode;
using engine::modpack::ModSource;
using engine::modpack::ModSourceDirect;
using engine::modpack::ModSourceEmbedded;
using engine::modpack::ModSourceLoversLab;
using engine::modpack::ModSourceModPub;
using engine::modpack::ModSourceNexus;
using engine::modpack::ModSourceSteamWorkshop;
using engine::modpack::parse_semver;
using engine::modpack::PatchChain;
using engine::modpack::PatchEntry;
using engine::modpack::PlatformBlock;
using engine::modpack::PlatformOverride;
using engine::modpack::PlatformPrefixFile;
using engine::modpack::SemverParse;
using engine::modpack::SeparatorNode;
using engine::modpack::TreeNode;
using engine::modpack::TreeRoot;

}  // namespace engine::gmmpack