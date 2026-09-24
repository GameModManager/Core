#pragma once

// Pack adapter interface - the abstraction that lets every pack format drive
// the same installer UI.
//
// A pack is either file-based (a .gmmpack archive on disk) or API-based (a
// Nexus collection reached through an nxm:// URL, resolved over the Nexus
// API - never as a raw file). Each format implements Pack::Interface; the
// installer wizard talks only through this interface and never parses a
// format directly.
//
// Engine layer - Qt-free (only <string>, <vector>, <memory>, <cstdint>).

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine::Pack {

// ---------------------------------------------------------------------------
// Source kinds
// ---------------------------------------------------------------------------

// Where a pack reference comes from:
//   File   - a local .gmmpack archive path.
//   NxmApi - an nxm:// URL for a Nexus collection, resolved via the Nexus API.
enum class SourceKind {
  File,
  NxmApi,
};

// Classify a pack reference (file path or URL). Unknown references report
// File when they end in .gmmpack (case-insensitive) and NxmApi when they
// start with nxm:// (case-insensitive); anything else defaults to File so a
// plain path still routes to the file adapter, which then reports the error.
[[nodiscard]] SourceKind detect_source_kind(const std::string &url_or_path);

// True when any pack adapter can handle the reference (a .gmmpack path or an
// nxm:// URL).
[[nodiscard]] bool can_handle(const std::string &url_or_path);

// ---------------------------------------------------------------------------
// Outcome - tiny success/error carrier (no exceptions across the interface)
// ---------------------------------------------------------------------------

template <typename T>
struct Outcome {
  bool ok = false;
  T value{};
  std::string error{};

  [[nodiscard]] static Outcome success(T v) {
    Outcome o;
    o.ok    = true;
    o.value = std::move(v);
    return o;
  }

  [[nodiscard]] static Outcome failure(std::string e) {
    Outcome o;
    o.ok    = false;
    o.error = std::move(e);
    return o;
  }
};

// ---------------------------------------------------------------------------
// Value types
// ---------------------------------------------------------------------------

// One mod listed in a pack manifest.
struct ModEntry {
  std::string id;           // stable id within the pack
  std::string name;         // human-readable name
  std::string version;      // pinned version (empty = latest)
  std::string source_type;  // e.g. "nexus", "loverslab", "direct"
  std::string source_id;    // provider-side id (mod id, URL, ...)
  std::string file_id;      // provider-side file id (empty = default file)
  bool optional = false;    // false = required
};

// Top-level pack identity plus its mod list.
struct PackManifest {
  std::string name;
  std::string author;
  std::string description;
  std::string game_id;   // GMM canonical game id
  std::string revision;  // pack revision (empty = unversioned)
  std::vector<ModEntry> mods;
};

// A manifest entry resolved to something downloadable.
struct ResolvedMod {
  std::string entry_id;      // ModEntry::id this was resolved from
  std::string display_name;  // human-readable mod/file name for the UI
  std::string archive_name;  // real archive filename (empty = provider default)
  std::string source_type;   // provider to download through
  std::string source_id;
  std::string file_id;
};

// What the installer needs to fetch a resolved mod.
struct DownloadInfo {
  std::string archive_name;     // real archive filename incl. extension
  std::string display_name;     // human-readable mod/file name for the UI
  std::string download_url;     // direct URL (empty = go through source provider)
  std::int64_t size_bytes = 0;  // 0 = unknown
};

// One node of the pack's install tree (separators, folders, mod leaves).
struct TreeNode {
  std::string id;
  std::string parent_id;  // empty = top level
  std::string name;
  bool is_separator = false;
};

// Install ordering / dependency constraint between two mods.
struct Rule {
  enum class Type {
    Before,
    After,
    Requires,
    Conflicts,
  };
  Type type = Type::After;
  std::string from;  // mod id
  std::string to;    // mod id
  std::string note;  // optional human note
};

// Binary patch applied to a file after install (e.g. xdelta/bsdiff).
struct Patch {
  std::string id;
  std::string target_path;  // game-relative path being patched
  std::string patch_path;   // pack-relative path of the patch data
};

// One INI tweak applied at install time.
struct IniEdit {
  std::string file;  // game-relative INI path
  std::string section;
  std::string key;
  std::string value;
};

// Launchable shipped or configured by the pack.
struct ExecEntry {
  std::string id;
  std::string name;
  std::string path;  // game-relative executable path
  std::string args;
};

// A headless-FOMOD choice: the installer pre-selects one option.
struct ChoiceGroup {
  enum class Mode {
    ExactlyOne,
    AtMostOne,
  };
  std::string id;
  std::string name;
  Mode mode = Mode::ExactlyOne;
  std::vector<std::string> options;
  std::string default_option;  // empty = no pre-selection
};

// ---------------------------------------------------------------------------
// Adapter interface
// ---------------------------------------------------------------------------

class Interface {
public:
  virtual ~Interface() = default;

  // Format id, e.g. "gmmpack" or "nexus-collection".
  [[nodiscard]] virtual std::string format_id() const = 0;

  // True when this adapter handles the reference (file path or nxm:// URL).
  [[nodiscard]] virtual bool
  can_handle_source(const std::string &url_or_path) const = 0;

  // Load and validate the pack, returning its manifest.
  [[nodiscard]] virtual Outcome<PackManifest>
  resolve_pack(const std::string &url_or_path) = 0;

  // Resolve one manifest entry to something downloadable.
  [[nodiscard]] virtual Outcome<ResolvedMod> resolve_mod(const ModEntry &entry) = 0;

  // Download details for a resolved mod (pure query over cached state).
  [[nodiscard]] virtual DownloadInfo
  get_download_info(const ResolvedMod &resolved) const = 0;

  // Pack content below the manifest: tree, rules, patches, INI edits,
  // executables, choice groups. Empty when the pack ships none.
  [[nodiscard]] virtual std::vector<TreeNode> get_tree() const             = 0;
  [[nodiscard]] virtual std::vector<Rule> get_rules() const                = 0;
  [[nodiscard]] virtual std::vector<Patch> get_patches() const             = 0;
  [[nodiscard]] virtual std::vector<IniEdit> get_ini_edits() const         = 0;
  [[nodiscard]] virtual std::vector<ExecEntry> get_executables() const     = 0;
  [[nodiscard]] virtual std::vector<ChoiceGroup> get_choice_groups() const = 0;
};

// ---------------------------------------------------------------------------
// Registry - routes a pack reference to the adapter that handles it
// ---------------------------------------------------------------------------

class Registry {
public:
  Registry() = default;
  void register_adapter(std::unique_ptr<Interface> adapter);
  // First adapter whose can_handle_source() accepts the reference, or
  // nullptr when no adapter handles it.
  Interface *adapter_for(const std::string &url_or_path) const;
  std::vector<Interface *> adapters() const;

private:
  std::vector<std::unique_ptr<Interface>> adapters_;
};

}  // namespace engine::Pack
