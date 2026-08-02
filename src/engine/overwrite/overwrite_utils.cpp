#include "engine/overwrite/overwrite_utils.h"

#include "engine/fs_utils.h"
#include "engine/index/conflict_engine.h"
#include "engine/log/logger.h"
#include "engine/meta/mod_meta.h"

#include <algorithm>
#include <set>
#include <system_error>

namespace engine {

namespace {

// Normalize a relative path string to forward slashes, no trailing slash.
std::string normalize_rel(std::string p) {
    std::replace(p.begin(), p.end(), '\\', '/');
    while (!p.empty() && p.front() == '/')
        p.erase(0, 1);
    while (!p.empty() && p.back() == '/')
        p.pop_back();
    return p;
}

// True when path starts with the segment prefix ("prefix/..." or equals it).
// ASCII case-insensitive: Isaac's game dir writes "Mods/" while the knowledge
// registry says "mods", and Windows filesystems ignore case.
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

// Strip a leading segment prefix ("prefix/rest" -> "rest", "prefix" -> "").
std::string strip_segment(const std::string& path, const std::string& prefix) {
    if (path.size() <= prefix.size()) return {};
    return path.substr(prefix.size() + 1);
}

// Rename src -> dst, falling back to copy+remove across devices. Returns true
// on success. On failure after a partial copy the destination is removed.
bool move_file_robust(const std::filesystem::path& src,
                      const std::filesystem::path& dst,
                      std::error_code& ec) {
    std::filesystem::create_directories(dst.parent_path(), ec);
    if (ec) return false;

    std::error_code move_ec;
    std::filesystem::rename(src, dst, move_ec);
    if (!move_ec) return true;

    move_ec.clear();
    std::filesystem::copy_file(src, dst,
        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        std::filesystem::remove(dst, move_ec);
        return false;
    }
    std::filesystem::remove(src, move_ec);
    return true;
}

// Prune empty directories walking up from `start` until (and excluding)
// `root`. Never removes root itself.
void prune_up(const std::filesystem::path& start,
              const std::filesystem::path& root) {
    std::error_code ec;
    auto dir = start;
    while (dir != root && std::filesystem::is_directory(dir, ec) &&
           !ec) {
        auto parent = dir.parent_path();
        if (!std::filesystem::is_empty(dir, ec) || ec) break;
        ec.clear();
        std::filesystem::remove(dir, ec);
        if (ec) break;
        ec.clear();
        dir = parent;
    }
}

// Is `dir` (a top-level overwrite entry) a mod-mapping root, i.e. the
// mods_subpath? Matches MO2's getModMappings-key semantics (case-insensitive).
bool is_mapping_root(const std::filesystem::path& entry,
                     const std::string& mods_subpath) {
    auto normalized = normalize_rel(mods_subpath);
    if (normalized.empty()) return false;
    auto first_segment = normalized.substr(0, normalized.find('/'));
    auto name = entry.filename().string();
    std::transform(name.begin(), name.end(), name.begin(), ::tolower);
    std::transform(first_segment.begin(), first_segment.end(),
                   first_segment.begin(), ::tolower);
    return name == first_segment;
}

// Recursive half of move_overwrite_to_mod: move every entry under `src` into
// mod_root, applying the prefix-strip to the full game-root-relative path each
// time. mod_root is always the original destination root (never an accumulated
// nested dir) - that is what makes the strip idempotent. Empty overwrite dirs
// are pruned afterwards.
bool move_entry_recursive(const std::filesystem::path& src,
                          const std::filesystem::path& overwrite_root,
                          const std::filesystem::path& mod_root,
                          const std::string& mods_subpath,
                          bool include_mod_id,
                          const std::string& mod_id) {
    std::error_code ec;
    if (!std::filesystem::is_directory(src, ec) || ec) return false;

    // Gather children first - moving during iteration invalidates iterators.
    std::vector<std::filesystem::path> children;
    std::filesystem::directory_iterator it(src, ec);
    auto end = std::filesystem::directory_iterator();
    while (it != end && !ec) {
        children.push_back(it->path());
        it.increment(ec);
    }
    if (ec) return false;

    bool ok = true;
    for (const auto& child : children) {
        auto rel = std::filesystem::relative(child, overwrite_root, ec);
        if (ec) {
            ec.clear();
            ok = false;
            continue;
        }
        auto mod_rel = overwrite_to_mod_rel(normalize_rel(rel.string()),
                                            mods_subpath, include_mod_id, mod_id);
        if (mod_rel.empty() && std::filesystem::is_directory(child, ec)) {
            // The whole dir maps onto the mod root (Skyrim "Data"): descend
            // without creating anything - the child paths are still mapped
            // from the overwrite root on the way down.
            if (ec) {
                ec.clear();
                ok = false;
                continue;
            }
            if (!move_entry_recursive(child, overwrite_root, mod_root,
                                      mods_subpath, include_mod_id, mod_id))
                ok = false;
            continue;
        }

        auto dest = mod_root / std::filesystem::path(mod_rel);

        if (std::filesystem::is_directory(child, ec)) {
            if (ec) {
                ec.clear();
                ok = false;
                continue;
            }
            std::filesystem::create_directories(dest, ec);
            if (ec) {
                ec.clear();
                ok = false;
                continue;
            }
            if (!move_entry_recursive(child, overwrite_root, mod_root,
                                      mods_subpath, include_mod_id, mod_id))
                ok = false;
        } else if (std::filesystem::is_regular_file(child, ec) && !ec) {
            if (!move_file_robust(child, dest, ec)) {
                Logger::instance().error(
                    "overwrite_utils: failed to move " + child.string() +
                    " -> " + dest.string() + ": " + ec.message());
                ec.clear();
                ok = false;
            }
        } else {
            ec.clear();
        }
    }

    prune_up(src, overwrite_root);
    return ok;
}

// Move a single overwrite file (game-root-relative) into dest_mod_dir,
// applying the prefix-strip. Removes an existing destination first.
bool sync_file(const std::filesystem::path& overwrite_dir,
               const std::string& overwrite_rel,
               const std::filesystem::path& dest_mod_dir,
               const std::string& mods_subpath,
               bool include_mod_id,
               const std::string& mod_id) {
    std::error_code ec;
    auto src = overwrite_dir / std::filesystem::path(overwrite_rel);
    if (!std::filesystem::is_regular_file(src, ec) || ec) {
        ec.clear();
        return false;
    }

    auto mod_rel = overwrite_to_mod_rel(overwrite_rel, mods_subpath,
                                        include_mod_id, mod_id);
    auto dest = dest_mod_dir / std::filesystem::path(mod_rel);
    std::filesystem::create_directories(dest.parent_path(), ec);
    if (ec) {
        ec.clear();
        return false;
    }

    // MO2 SyncOverwriteDialog::applyTo: remove the existing destination, then
    // move the overwrite file over it.
    if (std::filesystem::exists(dest, ec) && !ec) {
        std::filesystem::remove(dest, ec);
        if (ec) {
            Logger::instance().error("overwrite_utils: failed to remove " +
                dest.string() + ": " + ec.message());
            ec.clear();
            return false;
        }
    }

    if (!move_file_robust(src, dest, ec)) {
        Logger::instance().error("overwrite_utils: failed to move " +
            src.string() + " -> " + dest.string() + ": " + ec.message());
        ec.clear();
        return false;
    }

    prune_up(src.parent_path(), overwrite_dir);
    return true;
}

}  // namespace

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

bool move_overwrite_to_mod(const std::filesystem::path& overwrite_dir,
                           const std::filesystem::path& mod_dir,
                           const std::string& mods_subpath,
                           bool include_mod_id,
                           const std::string& mod_id) {
    std::error_code ec;
    if (!std::filesystem::is_directory(overwrite_dir, ec) || ec) {
        ec.clear();
        return true;  // nothing to move
    }
    std::filesystem::create_directories(mod_dir, ec);
    if (ec) {
        Logger::instance().error("overwrite_utils: cannot create " +
            mod_dir.string() + ": " + ec.message());
        return false;
    }
    return move_entry_recursive(overwrite_dir, overwrite_dir, mod_dir,
                                mods_subpath, include_mod_id, mod_id);
}

bool move_overwrite_entry_to_mod(const std::filesystem::path& overwrite_dir,
                                 const std::filesystem::path& entry_path,
                                 const std::filesystem::path& mod_dir,
                                 const std::string& mods_subpath,
                                 bool include_mod_id,
                                 const std::string& mod_id) {
    std::error_code ec;

    // The entry must live under the overwrite dir.
    auto rel = std::filesystem::relative(entry_path, overwrite_dir, ec);
    if (ec || rel.empty() || rel == "..") {
        ec.clear();
        return false;
    }
    std::string rel_str = rel.string();
    if (rel_str == ".." || rel_str.rfind("../", 0) == 0) return false;

    std::filesystem::create_directories(mod_dir, ec);
    if (ec) {
        ec.clear();
        return false;
    }

    if (std::filesystem::is_directory(entry_path, ec)) {
        if (ec) {
            ec.clear();
            return false;
        }
        return move_entry_recursive(entry_path, overwrite_dir, mod_dir,
                                    mods_subpath, include_mod_id, mod_id);
    }
    if (ec) return false;

    auto mod_rel = overwrite_to_mod_rel(normalize_rel(rel_str), mods_subpath,
                                        include_mod_id, mod_id);
    if (mod_rel.empty()) return false;
    auto dest = mod_dir / std::filesystem::path(mod_rel);
    if (!move_file_robust(entry_path, dest, ec)) {
        Logger::instance().error("overwrite_utils: failed to move " +
            entry_path.string() + " -> " + dest.string() + ": " + ec.message());
        ec.clear();
        return false;
    }
    prune_up(entry_path.parent_path(), overwrite_dir);
    return true;
}

bool sync_overwrite_file(const std::filesystem::path& overwrite_dir,
                         const std::string& overwrite_rel,
                         const std::filesystem::path& dest_mod_dir,
                         const std::string& mods_subpath,
                         bool include_mod_id,
                         const std::string& mod_id) {
    return sync_file(overwrite_dir, overwrite_rel, dest_mod_dir, mods_subpath,
                     include_mod_id, mod_id);
}

bool overwrite_is_empty(const std::filesystem::path& overwrite_dir,
                        const std::string& mods_subpath) {
    std::error_code ec;
    if (!std::filesystem::is_directory(overwrite_dir, ec) || ec) {
        ec.clear();
        return true;
    }

    std::filesystem::directory_iterator it(overwrite_dir, ec);
    auto end = std::filesystem::directory_iterator();
    while (it != end && !ec) {
        const auto& entry = *it;
        auto name = entry.path().filename().string();

        if (entry.is_directory()) {
            if (is_mapping_root(entry.path(), mods_subpath)) {
                // An empty mapping root does not count as content.
                std::error_code sub_ec;
                bool has_content = false;
                std::filesystem::directory_iterator sub(entry.path(), sub_ec);
                auto sub_end = std::filesystem::directory_iterator();
                while (sub != sub_end && !sub_ec) {
                    has_content = true;
                    break;
                }
                if (has_content) return false;
            } else {
                return false;  // a real directory = content
            }
        } else if (entry.is_regular_file() && name != "meta.ini") {
            return false;
        }
        it.increment(ec);
        if (ec) ec.clear();
    }
    return true;
}

bool clear_overwrite(const std::filesystem::path& overwrite_dir,
                     const std::string& mods_subpath) {
    std::error_code ec;
    if (!std::filesystem::is_directory(overwrite_dir, ec) || ec) {
        ec.clear();
        return true;
    }

    bool ok = true;
    std::filesystem::directory_iterator it(overwrite_dir, ec);
    auto end = std::filesystem::directory_iterator();
    while (it != end && !ec) {
        const auto& entry = *it;
        if (entry.is_directory() && is_mapping_root(entry.path(), mods_subpath)) {
            // Keep the mapping root itself; delete its contents.
            std::error_code sub_ec;
            std::filesystem::directory_iterator sub(entry.path(), sub_ec);
            auto sub_end = std::filesystem::directory_iterator();
            while (sub != sub_end && !sub_ec) {
                if (!remove_path(sub->path())) ok = false;
                sub.increment(sub_ec);
                if (sub_ec) sub_ec.clear();
            }
        } else {
            if (!remove_path(entry.path())) ok = false;
        }
        it.increment(ec);
        if (ec) ec.clear();
    }
    return ok;
}

bool game_has_file(const std::filesystem::path& game_dir,
                   const std::string& overwrite_rel) {
    if (game_dir.empty() || overwrite_rel.empty()) return false;
    std::error_code ec;
    auto p = game_dir / std::filesystem::path(overwrite_rel);
    return std::filesystem::is_regular_file(p, ec) && !ec;
}

std::vector<OverwriteSyncFile> collect_overwrite_sync_files(
    const std::filesystem::path& overwrite_dir,
    const std::filesystem::path& mods_dir,
    const std::vector<std::pair<std::string, int>>& mod_infos,
    const std::string& mods_subpath,
    bool conflict_reversed,
    bool include_mod_id,
    const std::filesystem::path& game_dir) {
    std::vector<OverwriteSyncFile> result;

    std::error_code ec;
    if (!std::filesystem::is_directory(overwrite_dir, ec) || ec) {
        ec.clear();
        return result;
    }

    // Owners come from a fresh, unrestricted conflict compute - the sync
    // dialog must see every file, unlike the flags column's extension filter.
    engine::ConflictEngine engine;
    const auto computed = engine.compute(mods_dir, mod_infos, /*extensions_csv=*/"",
                                         /*ignored_csv=*/"", conflict_reversed,
                                         /*cache_path=*/{});
    (void)computed;
    const auto& registry = engine.last_registry();

    std::vector<std::string> files;
    {
        std::filesystem::recursive_directory_iterator rit(
            overwrite_dir,
            std::filesystem::directory_options::skip_permission_denied, ec);
        auto rend = std::filesystem::recursive_directory_iterator();
        while (rit != rend) {
            if (ec) {
                ec.clear();
                rit.increment(ec);
                continue;
            }
            if (rit->is_regular_file()) {
                auto rel = std::filesystem::relative(rit->path(), overwrite_dir, ec);
                if (!ec) files.push_back(normalize_rel(rel.string()));
            }
            rit.increment(ec);
        }
    }

    for (const auto& rel : files) {
        auto mod_rel = overwrite_to_mod_rel(rel, mods_subpath);
        if (include_mod_id && !mod_rel.empty()) {
            // Isaac-style: the deployed path is "<mods_subpath>/<mod_folder>/<rel>",
            // so the mod-relative key drops the mod-folder segment too.
            auto slash = mod_rel.find('/');
            if (slash != std::string::npos)
                mod_rel = mod_rel.substr(slash + 1);
            else
                mod_rel.clear();  // a bare "<mods_subpath>/<file>" has no mod owner
        }
        if (mod_rel.empty()) continue;

        OverwriteSyncFile f;
        f.overwrite_rel = rel;

        auto it = registry.find(mod_rel);
        if (it != registry.end()) {
            for (const auto& [mod_id, priority] : it->second) {
                f.owners.push_back({mod_id, priority});
            }
            if (conflict_reversed) {
                std::stable_sort(f.owners.begin(), f.owners.end(),
                    [](const OverwriteOwner& a, const OverwriteOwner& b) {
                        return a.priority < b.priority;
                    });
            } else {
                std::stable_sort(f.owners.begin(), f.owners.end(),
                    [](const OverwriteOwner& a, const OverwriteOwner& b) {
                        return a.priority > b.priority;
                    });
            }
        }

        f.game_has_file = game_has_file(game_dir, rel);
        result.push_back(std::move(f));
    }

    return result;
}

size_t apply_sync_plan(const std::vector<OverwriteSyncTarget>& targets,
                       const std::filesystem::path& overwrite_dir,
                       const std::filesystem::path& mods_dir,
                       const std::string& mods_subpath,
                       const std::string& metadata_file,
                       bool include_mod_id) {
    size_t moved = 0;

    std::set<std::string> ensured;
    for (const auto& target : targets) {
        if (target.mod_folder.empty()) continue;

        auto mod_dir = mods_dir / target.mod_folder;
        std::error_code ec;

        // First destination for a folder: create it + write the game metadata
        // file so the mod scanner picks it up (MO2 createMod semantics).
        if (!std::filesystem::is_directory(mod_dir, ec) || ec) {
            ec.clear();
            std::filesystem::create_directories(mod_dir, ec);
            if (ec) {
                Logger::instance().error(
                    "overwrite_utils: cannot create " + mod_dir.string() + ": " +
                    ec.message());
                continue;
            }
            if (ensured.insert(target.mod_folder).second && !metadata_file.empty()) {
                ModMeta::write_game_metadata(mod_dir, metadata_file,
                                             target.mod_folder, "1.0", "0");
            }
        }

        if (sync_overwrite_file(overwrite_dir, target.overwrite_rel, mod_dir,
                                mods_subpath, include_mod_id,
                                target.mod_folder)) {
            ++moved;
        }
    }
    return moved;
}

}  // namespace engine
