#include "engine/deploy/deploy_utils.h"
#include "engine/core/log/logger.h"
#include "engine/core/util/fs_utils.h"
#include "engine/core/vfs/path_resolver_registry.h"
#include "engine/deploy/interface.h"
#include "engine/deploy/overlay_fs_deploy.h"
#include "engine/deploy/strategy.h"
#include "engine/deploy/symlink.h"
#include "engine/mod/filetree/dir_file_tree.h"
#include "engine/mod/meta/mod_meta.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <system_error>
#include <thread>
#include <unordered_set>

namespace engine {

// Pull Deploy:: names into engine:: for internal use (short names).
using Deploy::OverlayFsDeploy;
using Deploy::Symlink;

bool is_executable_binary(const std::filesystem::path &path) {
  auto ext = path.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });

  if (ext == ".exe" || ext == ".elf" || ext == ".sh")
    return true;

  // Extensionless: sniff the magic. ELF binaries and #! scripts both need
  // real-file semantics; a plain data file with no extension does not.
  if (ext.empty()) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec))
      return false;
    std::ifstream in(path, std::ios::binary);
    if (!in)
      return false;
    char magic[2] = {};
    in.read(magic, 2);
    if (in.gcount() < 2)
      return false;
    if (static_cast<unsigned char>(magic[0]) == 0x7f &&
        static_cast<unsigned char>(magic[1]) == 0x45) // \x7f 'E' == ELF
      return true;
    if (magic[0] == '#' && magic[1] == '!') // shebang script
      return true;
  }
  return false;
}

// Persistent deploy ledger: target path -> source path of what's currently
// deployed, written as a TSV. In overlay mode it lives inside the staging dir
// (wiped together with .gmm_staging at session end, so the next launch is a
// full parallel deploy by design); in direct-symlink mode it lives at the
// instance root where the session-end wipe can't reach it, so owner changes
// across sessions are detected. Round-trips byte-exactly on Linux.
std::map<std::filesystem::path, std::filesystem::path>
load_deploy_ledger(const std::filesystem::path &ledger_file) {
  std::map<std::filesystem::path, std::filesystem::path> m;
  std::ifstream in(ledger_file);
  std::string line;
  while (std::getline(in, line)) {
    auto tab = line.find('\t');
    if (tab == std::string::npos)
      continue;
    m.emplace(std::filesystem::path(line.substr(0, tab)),
              std::filesystem::path(line.substr(tab + 1)));
  }
  return m;
}

namespace {

// One enabled mod, with its walked file list (relative path -> absolute
// source).
struct ModSnapshot {
  std::string folder;
  bool root_override = false;
  std::vector<std::pair<std::filesystem::path, std::filesystem::path>> files;
};

// Farm `n` indexed tasks over a small thread pool. num_threads == 0 selects
// hardware_concurrency (capped at 16); the index-order dispatch keeps the
// work items independent, so the callback needs no internal synchronization.
template <typename Fn>
void run_parallel(size_t n, unsigned int num_threads, Fn &&fn) {
  if (n == 0)
    return;
  unsigned int t = num_threads;
  if (t == 0)
    t = std::max(1u, std::thread::hardware_concurrency());
  if (t == 0)
    t = 1;
  t = std::min(t, 16u);
  t = std::min<unsigned int>(t, static_cast<unsigned int>(n));
  if (t <= 1) {
    for (size_t i = 0; i < n; ++i)
      fn(i);
    return;
  }
  std::atomic<size_t> next{0};
  std::vector<std::thread> pool;
  pool.reserve(t);
  for (unsigned int k = 0; k < t; ++k) {
    pool.emplace_back([&fn, &next, n]() {
      for (;;) {
        size_t i = next.fetch_add(1, std::memory_order_relaxed);
        if (i >= n)
          break;
        fn(i);
      }
    });
  }
  for (auto &th : pool)
    th.join();
}

// Persistent deploy ledger: target path -> source path of what's currently
// deployed, written as a TSV. In overlay mode it lives inside the staging dir
// (wiped together with .gmm_staging at session end, so the next launch is a
// full parallel deploy by design); in direct-symlink mode it lives at the
// instance root where the session-end wipe can't reach it, so owner changes
// across sessions are detected. Round-trips byte-exactly on Linux.
std::filesystem::path ledger_path(const std::filesystem::path &staging_dir) {
  return staging_dir / ".gmm_deploy_ledger";
}

void save_ledger(
    const std::filesystem::path &ledger_file,
    const std::map<std::filesystem::path, std::filesystem::path> &m) {
  std::error_code ec;
  std::filesystem::path tmpp = ledger_file;
  tmpp += ".tmp";
  std::ofstream out(tmpp, std::ios::trunc);
  if (!out)
    return;
  for (const auto &[t, s] : m)
    out << t.string() << '\t' << s.string() << '\n';
  out.flush();
  out.close();
  std::filesystem::rename(tmpp, ledger_file, ec);
}

// True when `p` is a strict descendant of `root` (same filesystem spelling).
// Guards the backup store against backing up into itself when a root-override
// mod ships a path that happens to collide with the Original_Files folder.
bool is_within(const std::filesystem::path &p,
               const std::filesystem::path &root) {
  const auto rel = p.lexically_relative(root);
  return !rel.empty() && rel != "." && *rel.begin() != "..";
}

// Original-file safety (direct mode only): before the strategy overwrites a
// target, move a real (non-symlink) file/dir that occupies it into the backup
// store at backup_root/<relative path>, so it is preserved and restorable.
// Targets the deploy already owns (present in old_ledger) are left alone: they
// are mod artifacts, and a game overwrite of one (e.g. Pandora replacing a
// deployed symlink with a generated .hkx) is derived data, not an original.
void backup_original(
    const std::filesystem::path &target,
    const std::filesystem::path &deploy_root,
    const std::filesystem::path &backup_root,
    const std::map<std::filesystem::path, std::filesystem::path> &old_ledger) {
  if (backup_root.empty() || is_within(target, backup_root))
    return;
  std::error_code ec;
  if (!std::filesystem::exists(target, ec) || ec)
    return;
  if (std::filesystem::is_symlink(target))
    return;
  if (old_ledger.count(target))
    return;

  const auto rel = target.lexically_relative(deploy_root);
  if (rel.empty())
    return;
  const auto backup = backup_root / rel;
  std::filesystem::create_directories(backup.parent_path(), ec);
  std::filesystem::rename(target, backup, ec);
  if (ec) {
    Logger::instance().error("deploy: failed to back up original " +
                             target.string() + " to " + backup.string() + ": " +
                             ec.message());
  }
}

// Remove a deployed artifact (symlink or copied executable) and, when an
// original was parked for it, restore it from backup_root to the same relative
// location. Used by the incremental remove pass (a disabled mod's files) and by
// remove_deployed_files. Returns false if any step failed.
bool remove_and_restore(const std::filesystem::path &target,
                        const std::filesystem::path &deploy_root,
                        const std::filesystem::path &backup_root) {
  bool ok = true;
  std::error_code ec;
  const auto st = std::filesystem::symlink_status(target, ec);
  if (ec) {
    ec.clear();
  } else if (st.type() != std::filesystem::file_type::not_found) {
    std::filesystem::remove_all(target, ec);
    if (ec) {
      Logger::instance().error("deploy: failed to remove " + target.string() +
                               ": " + ec.message());
      ok = false;
    }
  }
  if (backup_root.empty() || is_within(target, backup_root))
    return ok;

  const auto rel = target.lexically_relative(deploy_root);
  if (rel.empty())
    return ok;
  const auto backup = backup_root / rel;
  std::error_code rec;
  if (!std::filesystem::exists(backup, rec) || rec)
    return ok;
  std::filesystem::create_directories(target.parent_path(), rec);
  if (rec) {
    Logger::instance().error("deploy: failed to prepare restore dir for " +
                             target.string() + ": " + rec.message());
    return false;
  }
  std::filesystem::rename(backup, target, rec);
  if (rec) {
    Logger::instance().error("deploy: failed to restore original " +
                             backup.string() + " -> " + target.string() + ": " +
                             rec.message());
    ok = false;
  }
  return ok;
}

// Remove empty directories under root, deepest first. Only ever called on the
// backup store (our own folder) after restoring originals out of it; never on
// game_dir.
void prune_empty_dirs(const std::filesystem::path &root) {
  std::error_code ec;
  std::vector<std::filesystem::path> dirs;
  std::filesystem::recursive_directory_iterator it(
      root, std::filesystem::directory_options::skip_permission_denied, ec);
  const std::filesystem::recursive_directory_iterator end;
  for (; it != end; it.increment(ec)) {
    if (ec)
      break;
    std::error_code sec;
    if (it->is_directory(sec))
      dirs.push_back(it->path());
  }
  std::sort(dirs.begin(), dirs.end(),
            [](const std::filesystem::path &a, const std::filesystem::path &b) {
              return a.string().size() > b.string().size();
            });
  for (const auto &d : dirs) {
    std::error_code rec;
    if (std::filesystem::is_empty(d, rec))
      std::filesystem::remove(d, rec);
  }
}

} // namespace

std::size_t
add_case_insensitive_aliases(const std::filesystem::path &staging_dir) {
  std::error_code ec;
  if (!std::filesystem::exists(staging_dir, ec) || ec)
    return 0;

  std::size_t created = 0;
  std::filesystem::recursive_directory_iterator it(
      staging_dir, std::filesystem::directory_options::skip_permission_denied,
      ec);
  const std::filesystem::recursive_directory_iterator end;
  for (; it != end; it.increment(ec)) {
    if (ec)
      break;

    // Only real directories get aliases. Files and symlinks (every staged
    // file is a symlink to its mod copy) are skipped; symlinked
    // directories - none exist in a normal deploy - are never descended.
    std::error_code sec;
    const auto st = std::filesystem::symlink_status(it->path(), sec);
    if (sec || !std::filesystem::is_directory(st))
      continue;

    const std::string name = it->path().filename().string();
    std::string lower = name;
    std::transform(
        lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == name)
      continue;

    const auto alias = it->path().parent_path() / lower;
    std::error_code aec;
    const auto atype = std::filesystem::symlink_status(alias, aec).type();
    if (aec) {
      aec.clear();
    } else if (atype == std::filesystem::file_type::symlink) {
      // Stale generated alias (its canonical dir was removed by a
      // redeploy): replace it rather than leave a dangling entry.
      std::filesystem::remove(alias, aec);
      aec.clear();
    } else if (atype != std::filesystem::file_type::not_found) {
      continue; // real file/dir already owns the alias name: leave it
    }

    std::filesystem::create_symlink(name, alias, aec);
    if (aec) {
      Logger::instance().warn("case-insensitive alias: failed to create " +
                              alias.string() + " -> " + name + ": " +
                              aec.message());
      continue;
    }
    ++created;
  }
  if (created > 0)
    Logger::instance().debug("case-insensitive aliases created: " +
                             std::to_string(created));
  return created;
}

// Shared executor for both deploy modes. deploy_root is the base directory
// targets are rooted at (the overlay staging dir, or game_dir for the
// direct-symlink mode). strategy performs the per-file link/copy work and
// decides how a target is created; ledger_file is where the target -> source
// TSV persists across runs. When add_ci_aliases is true (overlay mode for
// case-insensitive games), lowercase directory alias symlinks are added after
// the link phase so a Windows game's lowercase spellings resolve on the
// case-sensitive overlay mount.
bool deploy_impl(const path &mods_dir, const path &deploy_root,
                 const std::string &deploy_prefix, bool deploy_include_mod_id,
                 const std::string &disable_mechanism, bool case_sensitive,
                 Deploy::Interface &strategy, const path &ledger_file,
                 bool add_ci_aliases, const path &backup_root,
                 unsigned int num_threads, const DeployProgressFn &progress) {
  std::error_code ec;
  if (!std::filesystem::is_directory(mods_dir, ec)) {
    Logger::instance().warn("deploy_all_enabled_mods: mods_dir not found: " +
                            mods_dir.string());
    return false;
  }

  // --- Phase 0: snapshot enabled mods (one listing + one sentinel stat +
  //     one meta.ini read per mod; cheap, single-threaded). ---------------
  std::vector<ModSnapshot> mods;
  for (const auto &entry : std::filesystem::directory_iterator(mods_dir, ec)) {
    if (!entry.is_directory())
      continue;

    auto folder = entry.path().filename().string();

    // Skip the special overwrite dir and merged dir
    if (folder == "Overwrite" || folder == "MERGED" || folder == ".merged")
      continue;

    // Check disable sentinel
    try {
      bool enabled = disable_mechanism.empty() ||
                     !std::filesystem::exists(entry.path() / disable_mechanism);
      if (!enabled)
        continue;
    } catch (const std::filesystem::filesystem_error &ex) {
      Logger::instance().warn(
          "deploy_all_enabled_mods: cannot check enable state for " + folder +
          ": " + ex.what());
      continue;
    }

    // Root-override mods (meta.ini [General] rootOverride) deploy into the
    // staging root - the game root - instead of the data dir. A leading
    // Data/ folder inside such a mod lands in Data/ naturally.
    bool root_override = false;
    {
      std::error_code meta_ec;
      auto meta_ini = entry.path() / "meta.ini";
      if (std::filesystem::is_regular_file(meta_ini, meta_ec)) {
        try {
          engine::ModMeta meta = engine::ModMeta::load_file(meta_ini);
          root_override = meta.get("General", "rootOverride") == "1";
        } catch (const std::exception &ex) {
          Logger::instance().warn(
              "deploy_all_enabled_mods: cannot read meta for " + folder + ": " +
              ex.what());
        }
      }
    }
    mods.push_back({folder, root_override, {}});
  }

  // Deterministic winner order: lexicographic folder order, LAST mod wins a
  // contested target. The sequential version let directory_iterator order
  // decide (arbitrary filesystem order); this fixes the seed of
  // nondeterminism while keeping "later mod wins".
  std::sort(mods.begin(), mods.end(),
            [](const ModSnapshot &a, const ModSnapshot &b) {
              return a.folder < b.folder;
            });

  // --- Phase A: walk every enabled mod's tree in parallel, collecting
  //     (rel, source) file pairs. Purely discovery - no deploy work.
  run_parallel(mods.size(), num_threads, [&](size_t i) {
    ModSnapshot &m = mods[i];
    const auto mod_dir = mods_dir / m.folder;

    // Walk through the unified tree (PLAN §19.4 P1.1) instead of a
    // per-context recursive_directory_iterator. DirectoryFileTree
    // population is permission-hardened (do_populate increments with ec, a
    // permission-denied subdirectory is skipped, not a SIGABRT), and a
    // symlinked subdirectory is skipped to match the legacy iterator,
    // which never descends into one.
    std::error_code dir_ec;
    if (!std::filesystem::is_directory(mod_dir, dir_ec)) {
      Logger::instance().warn("deploy_all_enabled_mods: error iterating " +
                              m.folder + ": not a directory");
      return;
    }
    auto tree =
        DirectoryFileTree::make_tree(mod_dir, NameCompare::CaseInsensitive,
                                     /*ignore_meta_ini=*/false);
    tree->walk([&](const std::string &prefix,
                   const FileTree::const_reference &entry) {
      if (!entry->is_file()) {
        auto dir = std::dynamic_pointer_cast<const DirectoryFileTree>(entry);
        if (dir != nullptr && dir->is_symlink())
          return FileTree::WalkReturn::Skip;
        return FileTree::WalkReturn::Continue;
      }
      // Hidden files (.gmmhidden here, .mohidden from MO2-imported
      // instances) and the disable sentinel must not reach the game.
      const std::string rel = prefix + entry->name();
      if (is_hidden_file(std::filesystem::path(entry->name())) ||
          rel == disable_mechanism) {
        return FileTree::WalkReturn::Continue;
      }
      m.files.emplace_back(std::filesystem::path(rel), mod_dir / rel);
      return FileTree::WalkReturn::Continue;
    });
    // Sort within the mod so CI-equal dir spellings merge into a
    // deterministic casing (first-seen wins).
    std::sort(m.files.begin(), m.files.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });
  });

  // --- Phase B: resolve the deterministic winner per final target.
  std::filesystem::create_directories(deploy_root, ec);
  if (ec) {
    Logger::instance().error(
        "deploy_all_enabled_mods: failed to create deploy root: " +
        ec.message());
    return false;
  }
  auto target_base = deploy_root / deploy_prefix;
  std::filesystem::create_directories(target_base, ec);

  auto deploy_root_for = [&](const ModSnapshot &m) -> std::filesystem::path {
    return m.root_override
               ? deploy_root
               : (deploy_include_mod_id ? target_base / m.folder : target_base);
  };

  std::map<std::filesystem::path, std::filesystem::path>
      winners; // target -> source

  // One PathResolver per deploy root: its index is a cache derived from the
  // on-disk tree (the ledger stays the source of truth). CI games route
  // directory resolution through it so dual-case mod dirs (Meshes/ + meshes/)
  // collapse into one on-disk casing, exactly as resolve_deploy_target_ci did.
  auto &resolver = vfs::PathResolverRegistry::instance().resolver(
      deploy_root, case_sensitive ? vfs::NameCompare::CaseSensitive
                                  : vfs::NameCompare::CaseInsensitive);
  if (case_sensitive) {
    for (const auto &m : mods) {
      auto base = deploy_root_for(m);
      for (const auto &[rel, source] : m.files)
        winners[base / rel] = source;
    }
  } else {
    // Case-insensitive games: the resolver matches existing directory
    // components, so CI-equal spellings (Meshes/ vs meshes/) merge into the
    // FIRST-created casing. Pre-create every unique parent directory
    // single-threaded in deterministic order BEFORE resolving targets, so
    // the later resolution sees the merged tree and the parallel deploy
    // never races two workers on the same on-disk directory.
    std::vector<std::filesystem::path> parents;
    {
      std::unordered_set<std::string> seen;
      for (const auto &m : mods) {
        auto base = deploy_root_for(m);
        for (const auto &[rel, src] : m.files) {
          auto p = (base / rel).parent_path().lexically_normal();
          if (seen.insert(p.string()).second)
            parents.push_back(p);
        }
      }
    }
    for (const auto &p : parents) {
      // resolve_dir leaves the last component unmatched; probe a sentinel
      // leaf so every real directory component is CI-matched, then drop
      // it. The resolved directory is the merged on-disk casing.
      auto rel_p = p.lexically_relative(deploy_root);
      auto resolved = resolver.resolve_dir((rel_p / ".gmmprobe").string());
      std::filesystem::create_directories(resolved, ec);
    }
    // CI-equal filenames (0_Master.hxk vs 0_master.hxk) must collapse to
    // exactly ONE staged file, not land side-by-side. Fold the whole
    // target to a case-insensitive map key (PathResolver::normalize, the
    // FULL variant: every component lowercased) so a collision erases the
    // earlier-written on-disk target; the last mod in lexicographic folder
    // order therefore wins the slot (matching the contested-target
    // contract above) and its casing is what lands on disk.
    std::map<std::string, std::filesystem::path> canonical; // fold -> target
    for (const auto &m : mods) {
      auto base = deploy_root_for(m);
      auto base_rel = base.lexically_relative(deploy_root);
      for (const auto &[rel, source] : m.files) {
        auto rel_against_root = base_rel / rel;
        const auto target = resolver.resolve_dir(rel_against_root.string()) /
                            std::filesystem::path(rel).filename();
        const std::string key = resolver.normalize(target.string());
        if (auto it = canonical.find(key); it != canonical.end())
          winners.erase(it->second);
        canonical[key] = target;
        winners[target] = source;
      }
    }
  }

  // --- Phase C: diff winners against the persisted ledger (O(Δ) redeploy).
  // Entries whose winner and staged file are unchanged cost one stat and are
  // skipped; entries that are new, re-pointed or missing are re-deployed;
  // entries that stopped being winners (disabled/removed mod) are unlinked
  // so a disabled mod's files can't linger in the overlay.
  struct WorkItem {
    std::filesystem::path target;
    std::filesystem::path source;
    bool remove;
  };
  std::vector<WorkItem> work;
  auto old_ledger = load_deploy_ledger(ledger_file);
  for (const auto &[target, source] : winners) {
    bool unchanged = false;
    if (auto it = old_ledger.find(target);
        it != old_ledger.end() && it->second == source) {
      std::error_code stec;
      unchanged = std::filesystem::exists(target, stec) && !stec;
    }
    if (!unchanged)
      work.push_back({target, source, false});
  }
  for (const auto &[target, src] : old_ledger) {
    (void)src;
    if (winners.find(target) == winners.end())
      work.push_back({target, {}, true});
  }

  // --- Phase D: parallel link/unlink over the work list.
  int work_failed = 0;
  if (!work.empty()) {
    std::atomic<int> done{0};
    std::atomic<int> failed{0};
    const int total = static_cast<int>(work.size());
    run_parallel(work.size(), num_threads, [&](size_t i) {
      const WorkItem &w = work[i];
      bool ok;
      if (w.remove) {
        // Unlink the deployed artifact and, in direct mode with a
        // backup store, restore the original it displaced (a disabled
        // mod's file returns the game to its pre-deploy state instead
        // of leaving a hole).
        ok = remove_and_restore(w.target, deploy_root, backup_root);
      } else {
        // Direct mode: park any real game file at the target before the
        // strategy overwrites it, so original files are never deleted.
        backup_original(w.target, deploy_root, backup_root, old_ledger);
        ok = strategy.deploy(w.source, w.target);
      }
      if (!ok)
        failed.fetch_add(1, std::memory_order_relaxed);
      int d = done.fetch_add(1, std::memory_order_relaxed) + 1;
      if (progress)
        progress(d, total);
    });
    work_failed = failed.load(std::memory_order_relaxed);
  } else if (progress) {
    progress(0, 0);
  }

  // --- Phase E: persist the ledger of what's now deployed.
  save_ledger(ledger_file, winners);

  // --- Phase F: case-insensitive alias pass (overlay mode, Windows games
  // only). The overlay mount is case-sensitive, but the game resolves paths
  // case-insensitively; lowercase symlink aliases make the staged tree
  // reachable under the spellings the game actually queries (Modex's
  // "data/interface/modex/...", OAR's "data/meshes/...") and funnel runtime
  // writes into one canonical casing instead of dual-case dirs. Direct mode
  // needs no aliases: the game reads its own (already-cased) game_dir, and
  // Wine/Proton resolve case-insensitively on their own.
  if (add_ci_aliases && !resolver.is_native_ci()) {
    const std::size_t aliases = add_case_insensitive_aliases(deploy_root);
    if (aliases == 0)
      Logger::instance().debug("case-insensitive aliases: none needed");
  }

  size_t file_count = 0;
  for (const auto &m : mods)
    file_count += m.files.size();
  Logger::instance().debug(
      "deploy_all_enabled_mods: " + std::to_string(mods.size()) +
      " mods processed, " + std::to_string(file_count) + " files staged, " +
      std::to_string(work.size()) + " touched (" + std::to_string(work_failed) +
      " failed)");
  return work_failed == 0;
}

bool deploy_all_enabled_mods_parallel(
    const path &mods_dir, const path &staging_dir,
    const std::string &deploy_prefix, bool deploy_include_mod_id,
    const std::string &disable_mechanism, bool case_sensitive,
    unsigned int num_threads, const DeployProgressFn &progress) {
  OverlayFsDeploy strategy(staging_dir, case_sensitive);
  return deploy_impl(mods_dir, staging_dir, deploy_prefix,
                     deploy_include_mod_id, disable_mechanism, case_sensitive,
                     strategy, ledger_path(staging_dir),
                     /*add_ci_aliases=*/!case_sensitive,
                     /*backup_root=*/{}, num_threads, progress);
}

bool deploy_all_enabled_mods_direct(
    const path &mods_dir, const path &game_dir,
    const std::string &deploy_prefix, bool deploy_include_mod_id,
    const std::string &disable_mechanism, bool case_sensitive,
    const path &ledger_file, const path &backup_root, unsigned int num_threads,
    const DeployProgressFn &progress) {
  Symlink strategy(case_sensitive);
  return deploy_impl(mods_dir, game_dir, deploy_prefix, deploy_include_mod_id,
                     disable_mechanism, case_sensitive, strategy, ledger_file,
                     /*add_ci_aliases=*/false, backup_root, num_threads,
                     progress);
}

bool remove_deployed_files(const path &game_dir, const path &backup_root,
                           const path &ledger_file, unsigned int num_threads,
                           const DeployProgressFn &progress) {
  auto ledger = load_deploy_ledger(ledger_file);
  if (ledger.empty()) {
    if (progress)
      progress(0, 0);
    return true;
  }

  std::vector<path> targets;
  targets.reserve(ledger.size());
  for (const auto &[target, src] : ledger) {
    (void)src;
    targets.push_back(target);
  }

  std::atomic<int> done{0};
  std::atomic<int> failed{0};
  const int total = static_cast<int>(targets.size());
  run_parallel(targets.size(), num_threads, [&](size_t i) {
    if (!remove_and_restore(targets[i], game_dir, backup_root))
      failed.fetch_add(1, std::memory_order_relaxed);
    int d = done.fetch_add(1, std::memory_order_relaxed) + 1;
    if (progress)
      progress(d, total);
  });

  // Restoring an original leaves its parent chain in the backup store empty;
  // prune those (backup_root is our own folder - never game_dir).
  if (!backup_root.empty())
    prune_empty_dirs(backup_root);

  if (failed.load(std::memory_order_relaxed) == 0) {
    // Everything restored/removed: drop the ledger so the next deploy
    // re-evaluates from scratch (and re-backs-up restored originals).
    std::error_code ec;
    std::filesystem::remove(ledger_file, ec);
    return true;
  }
  return false;
}

bool deploy_all_enabled_mods(const path &mods_dir, const path &staging_dir,
                             const std::string &deploy_prefix,
                             bool deploy_include_mod_id,
                             const std::string &disable_mechanism,
                             bool case_sensitive) {
  return deploy_all_enabled_mods_parallel(
      mods_dir, staging_dir, deploy_prefix, deploy_include_mod_id,
      disable_mechanism, case_sensitive, 1, nullptr);
}

} // namespace engine
