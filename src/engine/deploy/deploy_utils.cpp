#include "engine/deploy/deploy_utils.h"
#include "engine/deploy/strategy.h"
#include "engine/fs_utils.h"
#include "engine/log/logger.h"

#include <filesystem>
#include <system_error>

namespace engine {

bool deploy_all_enabled_mods(
    const path& mods_dir,
    const path& staging_dir,
    const std::string& deploy_prefix,
    bool deploy_include_mod_id,
    const std::string& disable_mechanism,
    bool case_sensitive)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(mods_dir, ec)) {
        Logger::instance().warn("deploy_all_enabled_mods: mods_dir not found: " + mods_dir.string());
        return false;
    }

    // Ensure staging root exists
    std::filesystem::create_directories(staging_dir, ec);
    if (ec) {
        Logger::instance().error("deploy_all_enabled_mods: failed to create staging dir: " + ec.message());
        return false;
    }

    OverlayFsDeployStrategy strategy(staging_dir, case_sensitive);
    auto target_base = staging_dir / deploy_prefix;
    std::filesystem::create_directories(target_base, ec);

    int total_deployed = 0;
    int total_failed = 0;
    int mods_processed = 0;

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

        // Deploy every file in the mod folder
        auto deploy_root = deploy_include_mod_id ? target_base / folder : target_base;
        int deployed = 0;
        int failed = 0;

        // Iterate with skip_permission_denied + explicit increment(ec).
        // A range-for over recursive_directory_iterator throws on the first
        // permission-denied subdirectory (the increment operator has no ec
        // overload), aborting the whole deploy with SIGABRT.
        std::error_code iter_ec;
        auto it = std::filesystem::recursive_directory_iterator(
            entry.path(),
            std::filesystem::directory_options::skip_permission_denied,
            iter_ec);
        auto end = std::filesystem::recursive_directory_iterator();
        while (it != end && !iter_ec) {
            const auto& file = *it;
            const auto rel = std::filesystem::relative(file.path(), entry.path());
            // Hidden files (.gmmhidden here, .mohidden from MO2-imported
            // instances) and the disable sentinel (disable_mechanism, e.g.
            // ".gmmdisabled" or Isaac's "disable.it") must not reach the game -
            // MO2 parity. The skip is a filter, not a continue: the iterator
            // must still advance.
            if (file.is_regular_file() && !is_hidden_file(file.path()) &&
                rel != disable_mechanism) {
                // NOTE: no create_directories(target.parent_path()) here. The
                // strategy owns dir creation, and pre-creating the target's
                // parent with the mod's exact casing would defeat
                // resolve_deploy_target_ci (the exact-cased dir would already
                // exist, so the case-insensitive merge could never fire).
                auto target = deploy_root / rel;
                if (strategy.deploy(file.path(), target)) {
                    ++deployed;
                } else {
                    ++failed;
                }
            }
            it.increment(iter_ec);
        }
        if (iter_ec) {
            Logger::instance().warn("deploy_all_enabled_mods: error iterating " + folder + ": " + iter_ec.message());
        }

        total_deployed += deployed;
        total_failed += failed;
        ++mods_processed;

        Logger::instance().debug("deploy_all_enabled_mods: " + folder +
            ": " + std::to_string(deployed) + " deployed" +
            (failed ? ", " + std::to_string(failed) + " failed" : ""));
    }

    Logger::instance().debug("deploy_all_enabled_mods: " + std::to_string(mods_processed) +
        " mods processed, " + std::to_string(total_deployed) + " files deployed, " +
        std::to_string(total_failed) + " failed");
    return total_failed == 0;
}

}
