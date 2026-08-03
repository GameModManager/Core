#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <system_error>

namespace engine {

// ---------------------------------------------------------------------------
// Windows-native path resolution
// ---------------------------------------------------------------------------
// Every path that comes from a mod or an archive is treated as Windows-native:
// backslash separators and arbitrary casing must resolve the same way they
// would on Windows, including on a case-sensitive filesystem (Linux/macOS).
// resolve_path() is the single canonical resolver for that input - never
// hand-roll per-site separator/case handling. See PLAN.md (FOMOD plugin:
// `case_insensitive` bool, default true).

// Lowercase a copy of the string.
[[nodiscard]] inline std::string toLower(std::string str)
{
    std::transform(str.begin(), str.end(), str.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return str;
}

// Translate Windows backslash separators to '/'.
[[nodiscard]] inline std::string normalize_separators(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

// True when path's filename matches name case-insensitively (either side may
// be any casing).
[[nodiscard]] inline bool name_matches_ci(const std::filesystem::path& p,
                                          const std::string& name)
{
    return toLower(p.filename().string()) == toLower(name);
}

// Case-insensitive lookup of a regular file by its lowercase name in a
// directory. Returns the real on-disk path or an empty path.
[[nodiscard]] inline std::filesystem::path find_file_ci(
    const std::filesystem::path& dir, const std::string& lowerName)
{
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) {
            return {};
        }
        if (entry.is_regular_file(ec) && name_matches_ci(entry.path(), lowerName)) {
            return entry.path();
        }
    }
    return {};
}

// Resolve a Windows-native relative path against a root directory. On Windows
// the OS already accepts both separators and matches case-insensitively, so
// the candidate is `root/relative` lexically normalized; on case-sensitive
// platforms each component is matched case-insensitively against the on-disk
// tree. Returns the real on-disk path (with the tree's actual casing) or an
// empty path when not found.
//
// Empty, absolute and ".." traversal paths are rejected without touching the
// filesystem (security is platform-independent); `escaped` is set to true for
// those rejection reasons so callers can skip the entry silently instead of
// reporting it missing. A plain absence leaves `escaped` false.
[[nodiscard]] inline std::filesystem::path resolve_path(
    const std::filesystem::path& root, const std::string& relative,
    bool* escaped = nullptr)
{
    if (escaped) {
        *escaped = false;
    }
    if (relative.empty()) {
        if (escaped) {
            *escaped = true;
        }
        return {};
    }

    const std::filesystem::path rel(normalize_separators(relative));
    if (rel.is_absolute()) {
        if (escaped) {
            *escaped = true;
        }
        return {};
    }
    for (const auto& part : rel) {
        if (part == "..") {
            if (escaped) {
                *escaped = true;
            }
            return {};
        }
    }

#if defined(_WIN32)
    std::error_code ec;
    const auto candidate = (root / rel).lexically_normal();
    if (!std::filesystem::exists(candidate, ec)) {
        return {};
    }
    return candidate;
#else
    std::filesystem::path cur = root;
    for (const auto& part : rel) {
        const std::string comp = part.string();
        if (comp.empty() || comp == ".") {
            continue;
        }
        const std::string lowerComp = toLower(comp);
        std::error_code ec;
        std::filesystem::path match;
        bool found = false;
        for (const auto& entry : std::filesystem::directory_iterator(cur, ec)) {
            if (ec) {
                return {};
            }
            const std::string name = entry.path().filename().string();
            if (name == comp || toLower(name) == lowerComp) {
                match = entry.path();
                found = true;
                break;
            }
        }
        if (!found) {
            return {};
        }
        cur = match;
    }
    return cur;
#endif
}

// Hidden-file markers. GMM hides a mod file by renaming it to <name>.gmmhidden;
// .mohidden is MO2's marker and is recognized so instances shared with MO2 hide
// the same files. Both suffixes are skipped by deployment and shown as hidden
// in the Data tab.
inline constexpr const char* kGmmHiddenSuffix = ".gmmhidden";
inline constexpr const char* kMo2HiddenSuffix = ".mohidden";

// True if the file is hidden by either marker suffix (.gmmhidden or .mohidden).
[[nodiscard]] bool is_hidden_file(const std::filesystem::path& path);

// Hide a file by renaming it to <name>.gmmhidden. No-op if already hidden.
// Returns true on success, false on failure (file kept intact).
bool hide_file(const std::filesystem::path& path);

// Un-hide a file by stripping whichever marker suffix it carries (.gmmhidden
// or .mohidden, restoring the original MO2-compatible name). No-op if not
// hidden. Returns true on success, false on failure.
bool unhide_file(const std::filesystem::path& path);

// Sanitize a directory name to be valid on disk (MO2's fixDirectoryName
// equivalent): Windows-invalid characters (`: < > " ? * | /`, plus `\` on
// Windows) and non-printable/control characters become '_'; leading dots and
// trailing dots/spaces are stripped; reserved Windows device names
// (con, prn, aux, nul, com1-9, lpt1-9) are prefixed with '_'.
// Returns the sanitized name, or empty if nothing usable remains.
[[nodiscard]] std::string sanitize_directory_name(std::string name);

// Remove a file or directory from disk.
//
// By default the path is moved to the platform trash bin (recoverable).
// Pass permanent=true to delete it irreversibly - a "permanently delete"
// user setting will drive this later. Returns true if the path was removed
// (or did not exist), false on failure.
//
// This is the single canonical removal function: every mod / separator /
// download removal in the app routes through it.
bool remove_path(const std::filesystem::path& path, bool permanent = false);

// Move a file or directory from source to dest.
//
// Uses a rename; on a cross-device error (EXDEV) falls back to a recursive
// copy followed by removal of the source, so moves work across filesystems.
// dest is overwritten if it already exists (rename semantics on POSIX); the
// caller is responsible for resolving a name conflict first if a prompt is
// wanted. Returns true on success, false on failure (source kept intact on
// failure).
bool move_path(const std::filesystem::path& source,
               const std::filesystem::path& dest);

// Relay a per-session captured output dir into a mod folder.
//
// scratch_dir holds game-root-relative files captured during a single
// "Output to mod" launch. Files that map into the mod are moved into mod_dir
// (Data-relative layout - the mods_subpath / mod_id prefix is stripped);
// everything else is moved into overwrite_dir as leftover. The scratch dir is
// emptied of remaining directories afterwards (it is NOT removed itself - the
// caller owns its lifetime).
//
// Mapping rules (mirror the game plugin's on-disk layout):
//  - Skyrim-style (include_mod_id=false): scratch/<mods_subpath>/<rest>
//    maps to mod_dir/<rest>  (mods_subpath is e.g. "Data").
//  - Isaac-style (include_mod_id=true): scratch/<mods_subpath>/<mod_id>/<rest>
//    maps to mod_dir/<rest>  (mod_id is the mod's folder name).
// Files not under mods_subpath (or under a different mod's folder) go to
// overwrite_dir keeping their game-root-relative path.
//
// Returns the number of files relayed into mod_dir.
size_t relay_output_to_mod(const std::filesystem::path& scratch_dir,
                           const std::filesystem::path& mod_dir,
                           const std::filesystem::path& overwrite_dir,
                           const std::string& mods_subpath,
                           bool include_mod_id,
                           const std::string& mod_id);

}  // namespace engine
