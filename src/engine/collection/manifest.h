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
};

enum class ChoiceMode {
    ExactlyOne,
    AtMostOne,
};

enum class UpdatePolicy {
    Exact,
    Latest,
};

enum class SourceResolution {
    Api,
    Browser,
    ClientSubscription, // Steam Workshop only
};

// ---------------------------------------------------------------------------
// Pack identity & info
// ---------------------------------------------------------------------------

struct PackInfo {
    std::string name;
    std::string author;
    std::string description;
    std::string game_id; // GMM canonical game ID (NOT a per-provider slug)
    std::string homepage;
    std::string created_at; // ISO 8601
    std::string updated_at; // ISO 8601
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
    std::string from; // mod or executable id
    std::string to;   // mod or executable id
    std::string note; // optional human note
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
    std::string id; // slug: ^[a-z0-9]+(-[a-z0-9]+)*$
    std::string name;
    ChoiceMode mode = ChoiceMode::ExactlyOne;
    std::vector<std::string> member_mod_ids; // >= 2 entries
};

// ---------------------------------------------------------------------------
// Installer choice replay (per-mod FOMOD selections)
// ---------------------------------------------------------------------------

struct InstallerChoices {
    std::string type; // installer engine id, e.g. "fomod"
    // step/group id -> selected option ids
    std::unordered_map<std::string, std::vector<std::string>> selections;
};

// ---------------------------------------------------------------------------
// Mod sources (provider-specific download identity)
// ---------------------------------------------------------------------------

struct SourceNexus {
    static constexpr const char* kProvider = "nexus";
    SourceResolution resolution = SourceResolution::Api;
    std::string game_domain;
    int64_t mod_id = 0;
    int64_t file_id = 0;    // required when exact
    std::string version;
    std::string file_name;
    int64_t file_size = 0;
    std::string sha256;
    UpdatePolicy update_policy = UpdatePolicy::Exact;
};

struct SourceLoversLab {
    static constexpr const char* kProvider = "loverslab";
    SourceResolution resolution = SourceResolution::Browser;
    std::string mod_id; // int or string in JSON
    std::string section_slug;
    std::string version;
    std::string file_name;
    std::string sha256;
    UpdatePolicy update_policy = UpdatePolicy::Exact;
};

struct SourceModPub {
    static constexpr const char* kProvider = "modpub";
    SourceResolution resolution = SourceResolution::Api;
    std::string mod_id; // int or string in JSON
    std::string version;
    std::string file_name;
    std::string sha256;
    UpdatePolicy update_policy = UpdatePolicy::Exact;
};

struct SourceSteamWorkshop {
    static constexpr const char* kProvider = "steam_workshop";
    SourceResolution resolution = SourceResolution::ClientSubscription;
    int64_t app_id = 0;
    int64_t workshop_item_id = 0;
    std::string version; // always unknown ahead of time
    // updatePolicy is always Latest -- platform cannot honor exact pinning
};

struct SourceDirect {
    static constexpr const char* kProvider = "direct";
    SourceResolution resolution = SourceResolution::Browser;
    std::string url;
    std::string version;
    std::string file_name;
    std::string sha256;
    UpdatePolicy update_policy = UpdatePolicy::Exact;
};

using ModSource = std::variant<
    SourceNexus,
    SourceLoversLab,
    SourceModPub,
    SourceSteamWorkshop,
    SourceDirect
>;

// ---------------------------------------------------------------------------
// Mod entry (one per mod in the collection)
// ---------------------------------------------------------------------------

struct ModEntry {
    std::string id;   // filesystem-safe slug, matches filename
    std::string name; // human-readable
    int phase = 0;    // coarse install-sequencing bucket
    ModCategory category = ModCategory::Optional;
    ModSource source;
    InstallerChoices installer_choices; // may be empty (no FOMOD)
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
};

} // namespace engine::Collection
