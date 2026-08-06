#include "engine/deploy/deploy_utils.h"
#include "engine/deploy/strategy.h"
#include "engine/fs_utils.h"
#include "engine/log/logger.h"
#include "engine/meta/mod_meta.h"

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

bool is_executable_binary(const std::filesystem::path& path) {
    auto ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (ext == ".exe" || ext == ".elf" || ext == ".sh")
        return true;

    // Extensionless: sniff the magic. ELF binaries and #! scripts both need
    // real-file semantics; a plain data file with no extension does not.
    if (ext.empty()) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec)) return false;
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        char magic[2] = {};
        in.read(magic, 2);
        if (in.gcount() < 2) return false;
        if (static_cast<unsigned char>(magic[0]) == 0x7f &&
            static_cast<unsigned char>(magic[1]) == 0x45)  // \x7f 'E' == ELF
            return true;
        if (magic[0] == '#' && magic[1] == '!')  // shebang script
            return true;
    }
    return false;
}

namespace {

// One enabled mod, with its walked file list (relative path -> absolute source).
struct ModSnapshot {
    std::string folder;
    bool root_override = false;
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> files;
};

// Farm `n` indexed tasks over a small thread pool. num_threads == 0 selects
// hardware_concurrency (capped at 16); the index-order dispatch keeps the
// work items independent, so the callback needs no internal synchronization.
template <typename Fn>
void run_parallel(size_t n, unsigned int num_threads, Fn&& fn) {
    if (n == 0) return;
    unsigned int t = num_threads;
    if (t == 0) t = std::max(1u, std::thread::hardware_concurrency());
    if (t == 0) t = 1;
    t = std::min(t, 16u);
    t = std::min<unsigned int>(t, static_cast<unsigned int>(n));
    if (t <= 1) {
        for (size_t i = 0; i < n; ++i) fn(i);
        return;
    }
    std::atomic<size_t> next{0};
    std::vector<std::thread> pool;
    pool.reserve(t);
    for (unsigned int k = 0; k < t; ++k) {
        pool.emplace_back([&fn, &next, n]() {
            for (;;) {
                size_t i = next.fetch_add(1, std::memory_order_relaxed);
                if (i >= n) break;
                fn(i);
            }
        });
    }
    for (auto& th : pool) th.join();
}

// Persistent deploy ledger: target path -> source path of what's currently
// staged, written as a TSV inside the staging dir (wiped together with
// .gmm_staging at session end). Round-trips byte-exactly on Linux.
std::filesystem::path ledger_path(const std::filesystem::path& staging_dir) {
    return staging_dir / ".gmm_deploy_ledger";
}

std::map<std::filesystem::path, std::filesystem::path> load_ledger(
    const std::filesystem::path& staging_dir) {
    std::map<std::filesystem::path, std::filesystem::path> m;
    std::ifstream in(ledger_path(staging_dir));
    std::string line;
    while (std::getline(in, line)) {
        auto tab = line.find('\t');
        if (tab == std::string::npos) continue;
        m.emplace(std::filesystem::path(line.substr(0, tab)),
                  std::filesystem::path(line.substr(tab + 1)));
    }
    return m;
}

void save_ledger(const std::filesystem::path& staging_dir,
                 const std::map<std::filesystem::path, std::filesystem::path>& m) {
    std::error_code ec;
    const auto target = ledger_path(staging_dir);
    std::filesystem::path tmpp = target;
    tmpp += ".tmp";
    std::ofstream out(tmpp, std::ios::trunc);
    if (!out) return;
    for (const auto& [t, s] : m)
        out << t.string() << '\t' << s.string() << '\n';
    out.flush();
    out.close();
    std::filesystem::rename(tmpp, target, ec);
}

}  // namespace

bool deploy_all_enabled_mods_parallel(
    const path& mods_dir,
    const path& staging_dir,
    const std::string& deploy_prefix,
    bool deploy_include_mod_id,
    const std::string& disable_mechanism,
    bool case_sensitive,
    unsigned int num_threads,
    const DeployProgressFn& progress)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(mods_dir, ec)) {
        Logger::instance().warn("deploy_all_enabled_mods: mods_dir not found: " + mods_dir.string());
        return false;
    }

    // --- Phase 0: snapshot enabled mods (one listing + one sentinel stat +
    //     one meta.ini read per mod; cheap, single-threaded). ---------------
    std::vector<ModSnapshot> mods;
    for (const auto& entry : std::filesystem::directory_iterator(mods_dir, ec)) {
        if (!entry.is_directory()) continue;

        auto folder = entry.path().filename().string();

        // Skip the special overwrite dir and merged dir
        if (folder == "Overwrite" || folder == "MERGED" || folder == ".merged")
            continue;

        // Check disable sentinel
        try {
            bool enabled = disable_mechanism.empty() || !std::filesystem::exists(entry.path() / disable_mechanism);
            if (!enabled) continue;
        } catch (const std::filesystem::filesystem_error& ex) {
            Logger::instance().warn("deploy_all_enabled_mods: cannot check enable state for " + folder + ": " + ex.what());
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
                } catch (const std::exception& ex) {
                    Logger::instance().warn("deploy_all_enabled_mods: cannot read meta for " + folder + ": " + ex.what());
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
              [](const ModSnapshot& a, const ModSnapshot& b) { return a.folder < b.folder; });

    // --- Phase A: walk every enabled mod's tree in parallel, collecting
    //     (rel, source) file pairs. Purely discovery - no deploy work.
    run_parallel(mods.size(), num_threads, [&](size_t i) {
        ModSnapshot& m = mods[i];
        // skip_permission_denied + explicit increment(ec): a range-for over
        // recursive_directory_iterator throws on the first permission-denied
        // subdirectory, aborting the whole deploy with SIGABRT.
        std::error_code iter_ec;
        auto it = std::filesystem::recursive_directory_iterator(
            mods_dir / m.folder,
            std::filesystem::directory_options::skip_permission_denied,
            iter_ec);
        auto end = std::filesystem::recursive_directory_iterator();
        while (it != end && !iter_ec) {
            const auto& file = *it;
            const auto rel = std::filesystem::relative(file.path(), mods_dir / m.folder);
            // Hidden files (.gmmhidden here, .mohidden from MO2-imported
            // instances) and the disable sentinel must not reach the game.
            // The skip is a filter, not a continue: the iterator must still
            // advance.
            if (file.is_regular_file() && !is_hidden_file(file.path()) &&
                rel != disable_mechanism) {
                m.files.emplace_back(rel, file.path());
            }
            it.increment(iter_ec);
        }
        if (iter_ec) {
            Logger::instance().warn("deploy_all_enabled_mods: error iterating " + m.folder + ": " + iter_ec.message());
        }
        // Sort within the mod so CI-equal dir spellings merge into a
        // deterministic casing (first-seen wins).
        std::sort(m.files.begin(), m.files.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
    });

    // --- Phase B: resolve the deterministic winner per final target.
    std::filesystem::create_directories(staging_dir, ec);
    if (ec) {
        Logger::instance().error("deploy_all_enabled_mods: failed to create staging dir: " + ec.message());
        return false;
    }
    auto target_base = staging_dir / deploy_prefix;
    std::filesystem::create_directories(target_base, ec);

    auto deploy_root_for = [&](const ModSnapshot& m) -> std::filesystem::path {
        return m.root_override
            ? staging_dir
            : (deploy_include_mod_id ? target_base / m.folder : target_base);
    };

    std::map<std::filesystem::path, std::filesystem::path> winners;  // target -> source
    if (case_sensitive) {
        for (const auto& m : mods) {
            auto base = deploy_root_for(m);
            for (const auto& [rel, source] : m.files)
                winners[base / rel] = source;
        }
    } else {
        // Case-insensitive games: resolve_deploy_target_ci only matches
        // existing directory components, so CI-equal spellings (Meshes/ vs
        // meshes/) merge into the FIRST-created casing. Pre-create every
        // unique parent directory single-threaded in deterministic order
        // BEFORE resolving targets, so the later resolution sees the merged
        // tree and the parallel deploy never races two workers on the same
        // on-disk directory.
        std::vector<std::filesystem::path> parents;
        {
            std::unordered_set<std::string> seen;
            for (const auto& m : mods) {
                auto base = deploy_root_for(m);
                for (const auto& [rel, src] : m.files) {
                    auto p = (base / rel).parent_path().lexically_normal();
                    if (seen.insert(p.string()).second) parents.push_back(p);
                }
            }
        }
        for (const auto& p : parents) {
            // resolve_deploy_target_ci leaves the last component unmatched;
            // probe a sentinel leaf so every real component is CI-matched,
            // then drop it.
            auto resolved = resolve_deploy_target_ci(p / ".gmmprobe").parent_path();
            std::filesystem::create_directories(resolved, ec);
        }
        for (const auto& m : mods) {
            auto base = deploy_root_for(m);
            for (const auto& [rel, source] : m.files)
                winners[resolve_deploy_target_ci(base / rel)] = source;
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
    auto old_ledger = load_ledger(staging_dir);
    for (const auto& [target, source] : winners) {
        bool unchanged = false;
        if (auto it = old_ledger.find(target); it != old_ledger.end() && it->second == source) {
            std::error_code stec;
            unchanged = std::filesystem::exists(target, stec) && !stec;
        }
        if (!unchanged) work.push_back({target, source, false});
    }
    for (const auto& [target, src] : old_ledger) {
        (void)src;
        if (winners.find(target) == winners.end())
            work.push_back({target, {}, true});
    }

    // --- Phase D: parallel link/unlink over the work list.
    int work_failed = 0;
    if (!work.empty()) {
        OverlayFsDeployStrategy strategy(staging_dir, case_sensitive);
        std::atomic<int> done{0};
        std::atomic<int> failed{0};
        const int total = static_cast<int>(work.size());
        run_parallel(work.size(), num_threads, [&](size_t i) {
            const WorkItem& w = work[i];
            bool ok;
            if (w.remove) {
                std::error_code rmc;
                std::filesystem::remove(w.target, rmc);
                ok = !rmc;
            } else {
                ok = strategy.deploy(w.source, w.target);
            }
            if (!ok) failed.fetch_add(1, std::memory_order_relaxed);
            int d = done.fetch_add(1, std::memory_order_relaxed) + 1;
            if (progress) progress(d, total);
        });
        work_failed = failed.load(std::memory_order_relaxed);
    } else if (progress) {
        progress(0, 0);
    }

    // --- Phase E: persist the ledger of what's now staged.
    save_ledger(staging_dir, winners);

    size_t file_count = 0;
    for (const auto& m : mods)
        file_count += m.files.size();
    Logger::instance().debug("deploy_all_enabled_mods: " + std::to_string(mods.size()) +
        " mods processed, " + std::to_string(file_count) + " files staged, " +
        std::to_string(work.size()) + " touched (" +
        std::to_string(work_failed) + " failed)");
    return work_failed == 0;
}

bool deploy_all_enabled_mods(
    const path& mods_dir,
    const path& staging_dir,
    const std::string& deploy_prefix,
    bool deploy_include_mod_id,
    const std::string& disable_mechanism,
    bool case_sensitive)
{
    return deploy_all_enabled_mods_parallel(
        mods_dir, staging_dir, deploy_prefix, deploy_include_mod_id,
        disable_mechanism, case_sensitive, 1, nullptr);
}

}
