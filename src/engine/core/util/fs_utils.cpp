#include "engine/core/util/fs_utils.h"
#include "engine/core/log/logger.h"
#include "platform/platform.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <algorithm>
#include <cctype>
#include <cstring>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif

namespace engine {

// ---------------------------------------------------------------------------
// Game-root-relative -> mod-relative path bridge (shared with the overwrite
// sync dialog; see fs_utils.h).
// ---------------------------------------------------------------------------

std::string normalize_rel(std::string p) {
    std::replace(p.begin(), p.end(), '\\', '/');
    while (!p.empty() && p.front() == '/')
        p.erase(0, 1);
    while (!p.empty() && p.back() == '/')
        p.pop_back();
    return p;
}

bool starts_with_segment(const std::string& path, const std::string& prefix) {
    if (prefix.empty()) return false;
    if (path.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        char a = path[i], b = prefix[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + 32);
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b + 32);
        if (a != b) return false;
    }
    return path.size() == prefix.size() || path[prefix.size()] == '/';
}

std::string strip_segment(const std::string& path, const std::string& prefix) {
    if (path.size() <= prefix.size()) return {};
    return path.substr(prefix.size() + 1);
}

std::string overwrite_to_mod_rel(const std::string& overwrite_rel,
                                 const std::string& mods_subpath,
                                 bool include_mod_id,
                                 const std::string& mod_id) {
    auto rel = normalize_rel(overwrite_rel);
    auto subpath = normalize_rel(mods_subpath);
    if (subpath.empty()) return rel;

    if (starts_with_segment(rel, subpath)) {
        auto rest = strip_segment(rel, subpath);
        if (!include_mod_id || mod_id.empty()) return rest;
        if (starts_with_segment(rest, normalize_rel(mod_id)))
            return strip_segment(rest, normalize_rel(mod_id));
        return rest;
    }
    return rel;
}

std::string sanitize_directory_name(std::string name) {
    if (name.empty()) return {};

    const std::string invalid_chars = ":<>\"?*|/\\";
    for (auto& c : name) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (invalid_chars.find(c) != std::string::npos ||
            !std::isprint(uc)) {
            c = '\0';
        }
    }

    // Remove erased characters (replaced with null above).
    name.erase(std::remove(name.begin(), name.end(), '\0'), name.end());

    // Strip leading dots and trailing dots/spaces.
    while (!name.empty() && name.front() == '.')
        name.erase(0, 1);
    while (!name.empty() && (name.back() == '.' || name.back() == ' '))
        name.pop_back();

    if (name.empty()) return {};

    // Reserved Windows device names (case-insensitive).
    const auto is_reserved = [](const std::string& n) {
        static const char* const reserved[] = {
            "con", "prn", "aux", "nul",
            "com1", "com2", "com3", "com4", "com5",
            "com6", "com7", "com8", "com9",
            "lpt1", "lpt2", "lpt3", "lpt4", "lpt5",
            "lpt6", "lpt7", "lpt8", "lpt9"};
        for (const auto* r : reserved) {
            if (n.size() == std::strlen(r) &&
                std::equal(n.begin(), n.end(), r,
                           [](char a, char b) {
                               return std::tolower(static_cast<unsigned char>(a)) ==
                                      std::tolower(static_cast<unsigned char>(b));
                           })) {
                return true;
            }
        }
        return false;
    };
    if (is_reserved(name))
        name = "_" + name;

    return name;
}

namespace {

// Percent-encode a filesystem path for a freedesktop .trashinfo file.
std::string url_encode_path(const std::filesystem::path& path) {
    const std::string in = path.string();
    std::string out;
    out.reserve(in.size() + 8);
    for (unsigned char c : in) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '/' || c == '.' || c == '-' ||
            c == '_' || c == '~') {
            out += static_cast<char>(c);
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

std::string trash_root() {
    const char* data_home = std::getenv("XDG_DATA_HOME");
    if (data_home && *data_home)
        return std::string(data_home) + "/Trash";
    return safe_home_dir().string() + "/.local/share/Trash";
}

// freedesktop.org Trash spec (Linux).
bool move_to_trash_linux(const std::filesystem::path& path) {
    const auto trash = trash_root();
    if (trash.empty()) return false;

    const auto files_dir = std::filesystem::path(trash) / "files";
    const auto info_dir = std::filesystem::path(trash) / "info";

    std::error_code ec;
    std::filesystem::create_directories(files_dir, ec);
    std::filesystem::create_directories(info_dir, ec);
    if (ec) return false;

    // Collision-free name: name, name.1, name.2, ...
    const auto base = path.filename();
    auto target = files_dir / base;
    int n = 0;
    while (true) {
        ec.clear();
        if (!std::filesystem::exists(target, ec)) break;
        target = files_dir / (base.string() + "." + std::to_string(++n));
    }

    std::error_code move_ec;
    std::filesystem::rename(path, target, move_ec);
    if (move_ec) {
        // Cross-device fallback: copy recursively, then remove the source.
        std::error_code copy_ec;
        std::filesystem::copy(path, target,
            std::filesystem::copy_options::recursive |
            std::filesystem::copy_options::copy_symlinks, copy_ec);
        if (copy_ec) return false;
        std::filesystem::remove_all(path, ec);
    }

    // Write the .trashinfo sidecar so the entry can be restored.
    char date_buf[32];
    char offset_buf[16];
    const auto now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    std::strftime(date_buf, sizeof(date_buf), "%Y-%m-%dT%H:%M:%S", &tm);
    std::strftime(offset_buf, sizeof(offset_buf), "%z", &tm);  // +HHMM / -HHMM
    std::string offset(offset_buf);
    if (offset.size() == 5 && (offset[0] == '+' || offset[0] == '-'))
        offset = offset.substr(0, 3) + ":" + offset.substr(3);

    const auto info_path = info_dir / (target.filename().string() + ".trashinfo");
    std::ofstream info(info_path);
    if (!info) return false;
    info << "[Trash Info]\n"
         << "Path=" << url_encode_path(std::filesystem::absolute(path)) << "\n"
         << "DeletionDate=" << date_buf << offset << "\n";
    return true;
}

}  // namespace

bool remove_path(const std::filesystem::path& path, bool permanent) {
    if (path.empty()) return false;

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return true;  // already gone

    if (permanent) {
        std::filesystem::remove_all(path, ec);
        if (ec) {
            Logger::instance().error("Failed to permanently remove " +
                path.string() + ": " + ec.message());
            return false;
        }
        return true;
    }

#if defined(_WIN32)
    const std::wstring w = path.wstring();
    std::vector<wchar_t> from(w.begin(), w.end());
    from.push_back(L'\0');
    from.push_back(L'\0');
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.data();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    if (SHFileOperationW(&op) != 0) {
        Logger::instance().error("Failed to move " + path.string() +
            " to the Recycle Bin");
        return false;
    }
    return true;
#else
    return move_to_trash_linux(path);
#endif
}

bool move_path(const std::filesystem::path& source,
               const std::filesystem::path& dest) {
    std::error_code ec;

    // Plain rename first (fast path, same filesystem).
    std::filesystem::rename(source, dest, ec);
    if (!ec) return true;

    // Overwrite semantics: rename fails if dest exists on some platforms
    // (and the copy fallback below needs an explicit overwrite flag). Remove
    // a pre-existing dest so a conflict-resolved Overwrite is portable.
    ec.clear();
    if (std::filesystem::exists(dest, ec)) {
        ec.clear();
        std::filesystem::remove_all(dest, ec);
        if (!ec) {
            std::filesystem::rename(source, dest, ec);
            if (!ec) return true;
        }
    }

    // Cross-device fallback (EXDEV and friends): copy then remove the source.
    ec.clear();
    std::filesystem::copy(source, dest,
        std::filesystem::copy_options::recursive |
        std::filesystem::copy_options::copy_symlinks |
        std::filesystem::copy_options::overwrite_existing, ec);
    if (!ec)
        std::filesystem::remove_all(source, ec);
    if (ec) {
        Logger::instance().error("move_path: failed to move " + source.string() +
            " -> " + dest.string() + ": " + ec.message());
        return false;
    }
    return true;
}

namespace {

// Remove all empty directories under root (deepest first, repeatedly) so a
// relayed scratch dir leaves no skeleton behind.
void prune_empty_dirs(const std::filesystem::path& root) {
    std::error_code ec;
    bool any_removed = true;
    while (any_removed) {
        any_removed = false;
        std::filesystem::recursive_directory_iterator it(
            root, std::filesystem::directory_options::skip_permission_denied, ec);
        auto end = std::filesystem::recursive_directory_iterator();
        while (it != end && !ec) {
            auto& entry = *it;
            if (entry.is_directory() && !entry.is_symlink()) {
                auto dir = entry.path();
                if (std::filesystem::is_empty(dir, ec) && !ec) {
                    std::filesystem::remove(dir, ec);
                    if (!ec) any_removed = true;
                }
                ec.clear();
            }
            it.increment(ec);
            if (ec) {
                ec.clear();
                it = std::filesystem::recursive_directory_iterator(
                    root, std::filesystem::directory_options::skip_permission_denied, ec);
            }
        }
    }
}

}  // namespace

size_t relay_output_to_mod(const std::filesystem::path& scratch_dir,
                           const std::filesystem::path& mod_dir,
                           const std::filesystem::path& overwrite_dir,
                           const std::string& mods_subpath,
                           bool include_mod_id,
                           const std::string& mod_id) {
    (void)overwrite_dir;  // P2: ALL output goes into the mod, never Overwrite
    std::error_code ec;
    if (!std::filesystem::is_directory(scratch_dir, ec)) return 0;
    if (mod_dir.empty()) return 0;

    // Collect regular files first (moving during iteration invalidates it).
    std::vector<std::filesystem::path> files;
    {
        std::filesystem::recursive_directory_iterator it(
            scratch_dir, std::filesystem::directory_options::skip_permission_denied, ec);
        auto end = std::filesystem::recursive_directory_iterator();
        while (it != end) {
            if (ec) {
                ec.clear();
                it.increment(ec);
                continue;
            }
            if (it->is_regular_file() && !it->is_symlink())
                files.push_back(it->path());
            it.increment(ec);
        }
    }

    size_t relayed = 0;
    for (const auto& file : files) {
        auto rel = std::filesystem::relative(file, scratch_dir, ec);
        if (ec) {
            ec.clear();
            continue;
        }
        std::string rel_str = rel.string();
        std::replace(rel_str.begin(), rel_str.end(), '\\', '/');

        // Route EVERY file into the mod, mapping the scratch path through the
        // same game-root-relative -> mod-relative bridge the sync dialog uses
        // (overwrite_to_mod_rel): a Skyrim "Data/meshes/x" lands at mod
        // "meshes/x"; an Isaac "<mods_subpath>/<mod_id>/x" lands at mod "x";
        // a path outside the mapping (game-root file) passes through unchanged
        // and still lands in the mod. Nothing falls through to Overwrite.
        auto mod_rel = overwrite_to_mod_rel(rel_str, mods_subpath,
                                           include_mod_id, mod_id);
        if (mod_rel.empty()) {
            // A path that maps onto the mod root itself (bare "Data") — the
            // file is the mapping root: keep its name, drop nothing.
            mod_rel = rel_str;
        }

        std::filesystem::path dest = mod_dir / mod_rel;
        if (!dest.parent_path().empty())
            std::filesystem::create_directories(dest.parent_path(), ec);
        std::filesystem::rename(file, dest, ec);
        if (ec) {
            // Cross-device fallback: copy then remove the source.
            ec.clear();
            std::filesystem::copy_file(file, dest,
                std::filesystem::copy_options::overwrite_existing, ec);
            if (!ec)
                std::filesystem::remove(file, ec);
        }
        if (ec) {
            Logger::instance().error("relay_output_to_mod: failed to move " +
                file.string() + " -> " + dest.string() + ": " + ec.message());
            ec.clear();
        } else {
            ++relayed;
        }
    }

    prune_empty_dirs(scratch_dir);
    return relayed;
}

bool is_hidden_file(const std::filesystem::path& path) {
    const auto name = path.filename().string();
    const auto gmm = std::string(kGmmHiddenSuffix);
    const auto mo2 = std::string(kMo2HiddenSuffix);
    if (name.size() > gmm.size() &&
        name.compare(name.size() - gmm.size(), gmm.size(), gmm) == 0)
        return true;
    if (name.size() > mo2.size() &&
        name.compare(name.size() - mo2.size(), mo2.size(), mo2) == 0)
        return true;
    return false;
}

bool hide_file(const std::filesystem::path& path) {
    if (is_hidden_file(path)) return true;

    const auto hidden_path = path.string() + kGmmHiddenSuffix;
    std::error_code ec;
    std::filesystem::rename(path, hidden_path, ec);
    if (ec) {
        Logger::instance().error("hide_file: failed to rename " + path.string() +
            " -> " + hidden_path + ": " + ec.message());
        return false;
    }
    Logger::instance().debug("hide_file: " + path.string() +
        " -> " + hidden_path);
    return true;
}

bool unhide_file(const std::filesystem::path& path) {
    const auto name = path.filename().string();
    const auto gmm = std::string(kGmmHiddenSuffix);
    const auto mo2 = std::string(kMo2HiddenSuffix);

    std::string stem = name;
    if (name.size() > gmm.size() &&
        name.compare(name.size() - gmm.size(), gmm.size(), gmm) == 0)
        stem = name.substr(0, name.size() - gmm.size());
    else if (name.size() > mo2.size() &&
             name.compare(name.size() - mo2.size(), mo2.size(), mo2) == 0)
        stem = name.substr(0, name.size() - mo2.size());
    else
        return true;  // not hidden

    auto visible_path = path.parent_path() / stem;
    if (visible_path == path) return true;

    std::error_code ec;
    std::filesystem::rename(path, visible_path, ec);
    if (ec) {
        Logger::instance().error("unhide_file: failed to rename " + path.string() +
            " -> " + visible_path.string() + ": " + ec.message());
        return false;
    }
    Logger::instance().debug("unhide_file: " + path.string() +
        " -> " + visible_path.string());
    return true;
}

std::filesystem::path merged_view_file_resolve(
    const std::filesystem::path& game_dir,
    const std::filesystem::path& staging_dir,
    const std::filesystem::path& exec_path) {
    std::error_code ec;
    if (std::filesystem::exists(exec_path, ec)) return exec_path;
    if (staging_dir.empty() || game_dir.empty()) return {};

    // Resolution in the merged view covers three cases:
    //  1) the file physically exists (native game file, live overlay mount,
    //     or a legacy absolute path already resolved into the real mods
    //     folder) - handled above;
    //  2) it is a game-relative path that only exists once deployed into
    //     staging (e.g. a root-override mod's skse64_loader.exe);
    //  3) the same as 2 but reached through the ~/.steam symlink spelling,
    //     which weakly_canonical dissolves before the relative comparison.
    auto canon_exec = std::filesystem::weakly_canonical(exec_path, ec);
    if (ec || canon_exec.empty()) return {};
    ec.clear();
    auto canon_game = std::filesystem::weakly_canonical(game_dir, ec);
    if (ec || canon_game.empty()) return {};

    ec.clear();
    auto rel = std::filesystem::relative(canon_exec, canon_game, ec);
    if (ec || rel.empty() || rel.is_absolute()) return {};
    auto candidate = staging_dir / rel;
    if (std::filesystem::exists(candidate, ec)) return candidate;
    return {};
}

bool merged_view_file_exists(const std::filesystem::path& game_dir,
                             const std::filesystem::path& staging_dir,
                             const std::filesystem::path& exec_path) {
    return !merged_view_file_resolve(game_dir, staging_dir, exec_path).empty();
}

bool merged_view_executable_reachable(const std::filesystem::path& game_dir,
                                      const std::filesystem::path& staging_dir,
                                      const std::filesystem::path& exec_path) {
    auto resolved = merged_view_file_resolve(game_dir, staging_dir, exec_path);
    if (resolved.empty()) return false;
    std::error_code ec;
    // is_regular_file follows symlinks, so symlinked deploy strategies still
    // resolve to their target. Directories and special entries (e.g. a mod's
    // bin/ folder) are rejected - the launch chain cannot exec them.
    return std::filesystem::is_regular_file(resolved, ec);
}

}  // namespace engine
