#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>
#include <unordered_map>

namespace engine::gmmpack {

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
    std::string update_policy = "exact";
};

struct ModSourceLoversLab {
    std::string provider = "loverslab";
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
    std::string provider = "steam_workshop";
    std::string resolution = "client-subscription";
    int64_t app_id = 0;
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

using ModSource = std::variant<
    ModSourceNexus,
    ModSourceLoversLab,
    ModSourceModPub,
    ModSourceSteamWorkshop,
    ModSourceDirect
>;

struct InstallerChoices {
    std::string type;
    std::unordered_map<std::string, std::vector<std::string>> selections;
};

struct ModEntry {
    std::string id;
    std::string name;
    int phase = 0;
    std::string category;  // "required", "optional", "recommended"
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
    bool auto_run = false;
    bool rerun_on_modset_change = false;
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
};

// ---------------------------------------------------------------------------
// INI types (ini/<targetFile>.json)
//
// Mirrors the tweak shape in input/ini.schema.json. status is the raw
// "required"/"recommended" string; the typed enum lives in
// engine/modpack/ini_edits.h (see to_edit_file() in ini_edit_parser.h).
// ---------------------------------------------------------------------------

struct IniTweak {
    std::string id;  // stable slug; key for diffing/state/retract
    std::string name;  // human-readable label
    std::string status;  // "required", "recommended"
    bool enabled = true;  // author default
    std::string content;  // plain INI text
    std::string source_mod_id;  // empty = null (pack-author tweak)
    bool has_source_mod_id = false;
};

struct IniEntry {
    std::string target_file;
    std::vector<IniTweak> tweaks;
};

// ---------------------------------------------------------------------------
// Tree types (tree.json)
// ---------------------------------------------------------------------------

struct TreeNode;

struct SeparatorNode {
    std::string name;
    bool collapsed = false;
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
    std::vector<IniEntry> ini_edits;
    TreeRoot tree;
    std::optional<std::string> instructions;
};

}  // namespace engine::gmmpack
