#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace engine {

// ---------------------------------------------------------------------------
// Game-root-relative -> mod-relative path bridge
// ---------------------------------------------------------------------------
// Canonical, shared by relay_output_to_mod (the "Output to mod" relay) AND the
// overwrite sync dialog (Sync Overwrite to Mods). Lives here in fs_utils so a
// file captured at a game-root-relative path can be relocated into a flat mod
// folder without the relay pulling in the overwrite/conflict engine stack. See
// overwrite_utils.h for the path-space model.

// Normalize a relative path string to forward slashes, no trailing slash.
[[nodiscard]] std::string normalize_rel(std::string p);

// True when path starts with the segment prefix ("prefix/..." or equals it).
// ASCII case-insensitive: Isaac's game dir writes "Mods/" while the knowledge
// registry says "mods", and Windows filesystems ignore case.
[[nodiscard]] bool starts_with_segment(const std::string& path,
                                       const std::string& prefix);

// Strip a leading segment prefix ("prefix/rest" -> "rest", "prefix" -> "").
[[nodiscard]] std::string strip_segment(const std::string& path,
                                        const std::string& prefix);

// Normalize an overwrite-relative (game-root-relative) path into the
// mod-relative path the file should have inside a mod folder.
//
// Mirrors relay_output_to_mod's mapping rules:
//   - mods_subpath non-empty and path under "<mods_subpath>/" ->
//     strip "<mods_subpath>/" (Skyrim: "Data/meshes/x" -> "meshes/x").
//     The prefix match is case-insensitive: Isaac's game dir writes "Mods/"
//     while the knowledge registry says "mods".
//   - include_mod_id and path under "<mods_subpath>/<mod_id>/" ->
//     strip both (Isaac: "mods/MyMod/resources/x" -> "resources/x").
//   - otherwise the path is passed through unchanged.
[[nodiscard]] std::string overwrite_to_mod_rel(const std::string& overwrite_rel,
                                               const std::string& mods_subpath,
                                               bool include_mod_id = false,
                                               const std::string& mod_id = {});

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

// Normalize a relative path into the canonical spelling the conflict registry
// is keyed by: every DIRECTORY component lowercased, the final file name kept
// on its on-disk casing. This mirrors the deploy's case-insensitive directory
// merge (resolve_deploy_target_ci) exactly: CI-equal directory trees from
// Windows-game mods (Meshes/ + meshes/) register as the same deployed paths,
// while two CI-equal FILE names stay side-by-side (a rare packaging bug the
// deploy deliberately does not merge). Consumers that look a mod-relative
// path up in the registry must run it through this first.
[[nodiscard]] inline std::string normalize_ci_key(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    const auto last = path.find_last_of('/');
    if (last == std::string::npos) return path;
    std::string out;
    out.reserve(path.size());
    for (size_t i = 0; i < last; ++i)
        out += static_cast<char>(std::tolower(
            static_cast<unsigned char>(path[i])));
    out += '/';
    out += path.substr(last + 1);
    return out;
}

// Normalize a relative path into a FULLY case-insensitive key: every segment
// INCLUDING the final filename is lowercased (unlike normalize_ci_key, which
// keeps the final filename's on-disk casing). This is the ownership-only
// variant used by the Overwrite->mod association (MO2's shared tree findFile
// matches dirs AND the final name case-insensitively), so Data/Meshes/ReadMe.txt
// is owned by a mod storing meshes/readme.txt. The deploy and conflict-registry
// keys stay on normalize_ci_key (case-different file names remain distinct there);
// this narrow split is deliberate.
[[nodiscard]] inline std::string normalize_ci_full(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    std::transform(path.begin(), path.end(), path.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return path;
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

// Filter a plugin's comma-separated executable declarations down to the ones
// that physically exist under game_dir (detection only - never consults the
// deploy overlay; use merged_view_file_exists for that). Kept entries retain
// their declared game-relative spelling and declaration order (first = the
// default). A candidate counts as found when resolve_path() locates it AND it
// is launchable-shaped: a regular file, or - for macOS app bundles - a
// ".app"-suffixed directory. Missing names are silently dropped, so one
// declaration list doubles as a cross-platform candidate set: the scan itself
// is the platform filter.
[[nodiscard]] inline std::vector<std::string> filter_existing_executables(
    const std::filesystem::path& game_dir, const std::string& csv)
{
    std::vector<std::string> out;
    if (game_dir.empty() || csv.empty()) return out;
    std::istringstream ss(csv);
    std::string token;
    while (std::getline(ss, token, ',')) {
        const auto first = token.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        const auto last = token.find_last_not_of(" \t");
        const std::string name = token.substr(first, last - first + 1);
        bool escaped = false;
        const auto resolved = resolve_path(game_dir, name, &escaped);
        if (escaped || resolved.empty()) continue;
        std::error_code ec;
        const bool is_app = name.size() >= 4 &&
            toLower(name.substr(name.size() - 4)) == ".app";
        if ((std::filesystem::is_regular_file(resolved, ec) && !ec) ||
            (is_app && std::filesystem::is_directory(resolved, ec) && !ec)) {
            out.push_back(name);
        }
    }
    return out;
}

// Hidden-file markers. GMM hides a mod file by renaming it to <name>.gmmhidden;
// .mohidden is MO2's marker and is recognized so instances shared with MO2 hide
// the same files. Both suffixes are skipped by deployment and shown as hidden
// in the Data tab.
inline constexpr const char* kGmmHiddenSuffix = ".gmmhidden";
inline constexpr const char* kMo2HiddenSuffix = ".mohidden";

// True if the file is hidden by either marker suffix (.gmmhidden or .mohidden).
[[nodiscard]] bool is_hidden_file(const std::filesystem::path& path);

// Returns true when `exec_path` is reachable in the game's merged view:
// either physically on disk (native game file, live overlay mount, or a
// legacy absolute entry) or as a deployed mod file under `staging_dir`
// (game-relative paths only). `staging_dir` is typically the populated
// `.gmm_staging` dir; pass empty to skip the deploy fallback. Both paths
// are weakly-canonicalized first, so the ~/.steam vs ~/.local/share/Steam
// spelling mismatch never defeats the relative comparison.
[[nodiscard]] bool merged_view_file_exists(
    const std::filesystem::path& game_dir,
    const std::filesystem::path& staging_dir,
    const std::filesystem::path& exec_path);

// Returns the physical candidate that backs `exec_path` in the merged view:
// the host path itself when it exists, otherwise the deployed copy under
// `staging_dir` (game-relative paths only), otherwise empty. Same
// canonicalization rules as merged_view_file_exists.
[[nodiscard]] std::filesystem::path merged_view_file_resolve(
    const std::filesystem::path& game_dir,
    const std::filesystem::path& staging_dir,
    const std::filesystem::path& exec_path);

// True when `exec_path` resolves in the merged view to a regular file — the
// launchable-executable check. Rejects directories and special entries (e.g.
// a mod's bin/ folder) that the launchers cannot exec.
[[nodiscard]] bool merged_view_executable_reachable(
    const std::filesystem::path& game_dir,
    const std::filesystem::path& staging_dir,
    const std::filesystem::path& exec_path);

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
// "Output to mod" launch. EVERY captured file is moved into mod_dir -
// nothing falls through to overwrite_dir (P2: the output mod is the full
// write target, MO2 Custom Overwrites parity). overwrite_dir is ignored and
// kept only for API stability. The scratch dir is emptied of remaining
// directories afterwards (it is NOT removed itself - the caller owns its
// lifetime).
//
// Mapping rules (mirror the game plugin's on-disk layout, via the same
// overwrite_to_mod_rel bridge the sync dialog uses):
//  - Skyrim-style (include_mod_id=false): flat mods, a scratch
//    <mods_subpath>/<rest> file maps to mod_dir/<rest> (mods_subpath is e.g.
//    "Data"); a path outside the mapping passes through under its own name.
//  - Isaac-style (include_mod_id=true): scratch/<mods_subpath>/<mod_id>/<rest>
//    maps to mod_dir/<rest>  (mod_id is the mod's folder name).
// A path that maps onto the mod root itself (bare "Data") keeps its filename
// at the mod root.
//
// Returns the number of files relayed into mod_dir.
size_t relay_output_to_mod(const std::filesystem::path& scratch_dir,
                           const std::filesystem::path& mod_dir,
                           const std::filesystem::path& overwrite_dir,
                           const std::string& mods_subpath,
                           bool include_mod_id,
                           const std::string& mod_id);

}  // namespace engine
