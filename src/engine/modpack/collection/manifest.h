#pragma once

// Source-agnostic manifest schema for collections / modpacks.
//
// This header defines the universal data model that every collection source
// conforms to: Nexus collections, future GMM modpacks, manual imports, etc.
// It is a *schema* (pure value types), not a parser. Source-specific parsers
// (Nexus collection.json, .gmmpack archives) produce these types; consumers
// (rule engine, batch installer, download router) consume them.
//
// Engine layer - Qt-free.

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace engine::Collection {

// ---------------------------------------------------------------------------
// Enums
// ---------------------------------------------------------------------------

enum class ModCategory {
  Required,
  Optional,
  Recommended,
};

enum class RuleType {
  Before,
  After,
  Requires,
  Conflicts,
  Recommends,  // Vortex emits it; a soft hint, not an install gate
  Provides,    // Vortex emits it; one mod stands in for another
};

enum class ChoiceMode {
  ExactlyOne,
  AtMostOne,
};

// Three states, matching Nexus's UpdatePolicy enum: keep the pinned file
// (Exact), keep the pinned file but accept the mod's newest one when the
// pinned file was archived or deleted upstream (Prefer), or always take
// whatever the source currently reports as newest (Latest).
enum class UpdatePolicy {
  Exact,
  Prefer,
  Latest,
};

std::string_view to_string(UpdatePolicy policy);

// Inverse of to_string: "exact" / "prefer" / "latest". Anything else -
// including an absent value - is Exact, the most restrictive state.
UpdatePolicy parse_update_policy(std::string_view text);

enum class SourceResolution {
  Api,
  Browser,
  ClientSubscription,  // Steam Workshop only
};

// ---------------------------------------------------------------------------
// Pack identity & info
// ---------------------------------------------------------------------------

struct PackInfo {
  std::string name;
  std::string author;
  std::string description;
  std::string game_id;  // GMM canonical game ID (NOT a per-provider slug)
  std::string homepage;
  std::string created_at;  // ISO 8601
  std::string updated_at;  // ISO 8601
  // Free-form notes a collection author ships alongside the mod list.
  std::string install_instructions;
  std::vector<std::string> game_versions;
};

// ---------------------------------------------------------------------------
// External tool prerequisite (advisory only)
// ---------------------------------------------------------------------------

struct Tool {
  std::string id;
  std::string name;
  std::string homepage;
};

// ---------------------------------------------------------------------------
// Platform overrides
// ---------------------------------------------------------------------------

struct PrefixFile {
  std::string path;
  std::string source_mod_id;
};

struct PlatformOverride {
  std::string proton_version_pin;
  bool steam_overlay = false;
  std::string launch_options;
  std::vector<PrefixFile> prefix_files;
};

struct PlatformDefaults {
  PlatformOverride linux_;
  PlatformOverride macos;
  PlatformOverride windows;
};

// ---------------------------------------------------------------------------
// Install rules (dependency / ordering graph)
// ---------------------------------------------------------------------------

struct Rule {
  RuleType type = RuleType::Requires;
  std::string from;  // mod or executable id
  std::string to;    // mod or executable id
  std::string note;  // optional human note
};

// ---------------------------------------------------------------------------
// Plugin load order hints (weak tiebreak for LOOT)
// ---------------------------------------------------------------------------

struct LoadOrderHint {
  std::vector<std::string> plugin_hint;
};

// ---------------------------------------------------------------------------
// Choice groups (FOMOD replay)
// ---------------------------------------------------------------------------

struct ChoiceGroup {
  std::string id;  // slug: ^[a-z0-9]+(-[a-z0-9]+)*$
  std::string name;
  ChoiceMode mode = ChoiceMode::ExactlyOne;
  std::vector<std::string> member_mod_ids;  // >= 2 entries
};

// ---------------------------------------------------------------------------
// Installer choice replay (per-mod FOMOD selections)
// ---------------------------------------------------------------------------

struct InstallerChoices {
  std::string type;  // installer engine id, e.g. "fomod"
  // step/group id -> selected option ids
  std::unordered_map<std::string, std::vector<std::string>> selections;
};

// ---------------------------------------------------------------------------
// Mod sources (provider-specific download identity)
// ---------------------------------------------------------------------------

struct SourceNexus {
  static constexpr const char *kProvider = "nexus";
  SourceResolution resolution            = SourceResolution::Api;
  std::string game_domain;
  int64_t mod_id  = 0;
  int64_t file_id = 0;  // required when exact
  std::string version;
  std::string file_name;
  int64_t file_size = 0;
  // Nexus publishes MD5, not SHA-256. The digest lives in the field named
  // for its algorithm so a 32-hex MD5 never passes as a 64-hex SHA-256.
  std::string md5;
  std::string sha256;
  // Nexus's per-mod opaque identifier, carried through so a re-export points
  // at the same mod entry.
  std::string tag;
  UpdatePolicy update_policy = UpdatePolicy::Exact;
};

struct SourceLoversLab {
  static constexpr const char *kProvider = "loverslab";
  SourceResolution resolution            = SourceResolution::Browser;
  std::string mod_id;  // int or string in JSON
  std::string section_slug;
  std::string version;
  std::string file_name;
  std::string sha256;
  UpdatePolicy update_policy = UpdatePolicy::Exact;
};

struct SourceModPub {
  static constexpr const char *kProvider = "modpub";
  SourceResolution resolution            = SourceResolution::Api;
  std::string mod_id;  // int or string in JSON
  std::string version;
  std::string file_name;
  std::string sha256;
  UpdatePolicy update_policy = UpdatePolicy::Exact;
};

struct SourceSteamWorkshop {
  static constexpr const char *kProvider = "steam_workshop";
  SourceResolution resolution            = SourceResolution::ClientSubscription;
  int64_t app_id                         = 0;
  int64_t workshop_item_id               = 0;
  std::string version;  // always unknown ahead of time
  // updatePolicy is always Latest -- platform cannot honor exact pinning
};

struct SourceDirect {
  static constexpr const char *kProvider = "direct";
  SourceResolution resolution            = SourceResolution::Browser;
  std::string url;
  std::string version;
  std::string file_name;
  std::string md5;
  std::string sha256;
  UpdatePolicy update_policy = UpdatePolicy::Exact;
};

using ModSource = std::variant<SourceNexus, SourceLoversLab, SourceModPub,
                               SourceSteamWorkshop, SourceDirect>;

// ---------------------------------------------------------------------------
// Per-file install instructions (Nexus `hashes[]`)
// ---------------------------------------------------------------------------

// One installed file and the digest it must carry, as `path` + `md5`. This is
// the only per-file identity Nexus publishes for a collection mod, so it is
// what a replicate-style install has to check against.
struct FileHash {
  std::string path;
  std::string md5;
};

// ---------------------------------------------------------------------------
// Mod entry (one per mod in the collection)
// ---------------------------------------------------------------------------

struct ModEntry {
  std::string id;            // filesystem-safe slug, matches filename
  std::string name;          // human-readable
  int phase            = 0;  // coarse install-sequencing bucket
  ModCategory category = ModCategory::Optional;
  ModSource source;
  InstallerChoices installer_choices;  // may be empty (no FOMOD)
  std::vector<FileHash> hashes;        // per-file (path, md5); often empty
  std::string instructions;            // author note for this mod
};

// ---------------------------------------------------------------------------
// Unresolvable input, surfaced rather than dropped
// ---------------------------------------------------------------------------

// A rule (or any other declaration) the parser could not bind to a mod in the
// manifest. Kept so the UI can say what was ignored instead of silently
// importing a collection with fewer rules than it declared.
struct Unresolved {
  std::string what;    // "modRules[3]", "mods[7]"
  std::string reason;  // why it could not be resolved
};

// ---------------------------------------------------------------------------
// Archive integrity (file hashes for tamper detection)
// ---------------------------------------------------------------------------

struct ArchiveIntegrity {
  // archive-relative path -> "sha256:<hex>"
  std::unordered_map<std::string, std::string> file_hashes;
};

// ---------------------------------------------------------------------------
// Manifest (top-level collection descriptor)
// ---------------------------------------------------------------------------

struct Manifest {
  // Schema version (major must be 1 for v1.x.x)
  std::string schema_version;

  // Immutable pack identity + strictly increasing revision
  std::string id;  // UUID
  int64_t revision = 0;

  PackInfo info;
  std::vector<Tool> tools;
  PlatformDefaults platform;

  // Install graph
  std::vector<Rule> rules;
  LoadOrderHint load_order;
  std::vector<ChoiceGroup> choice_groups;

  // Mod list
  std::vector<ModEntry> mods;

  // Archive integrity
  ArchiveIntegrity archive;

  // Everything the format declared that we could not bind. Empty on a clean
  // parse. Never fatal: the import continues, and the UI reports these.
  std::vector<Unresolved> unresolved;
};

}  // namespace engine::Collection
