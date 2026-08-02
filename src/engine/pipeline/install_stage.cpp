#include "engine/pipeline/install_stage.h"
#include "engine/pipeline/pipeline.h"
#include "engine/model/mod.h"
#include "engine/instance/instance.h"
#include "engine/meta/mod_meta.h"
#include "engine/log/logger.h"

namespace engine {

static bool copy_recursive(const std::filesystem::path& src,
                            const std::filesystem::path& dst) {
    std::error_code ec;
    std::filesystem::create_directories(dst, ec);
    if (ec) return false;

    for (const auto& entry : std::filesystem::recursive_directory_iterator(src)) {
        auto relative = std::filesystem::relative(entry.path(), src);
        auto dest_path = dst / relative;

        if (entry.is_directory()) {
            std::filesystem::create_directories(dest_path, ec);
            if (ec) return false;
        } else if (entry.is_regular_file()) {
            std::filesystem::copy(entry.path(), dest_path,
                                  std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) return false;
        }
    }
    return true;
}

// MO2's generateBackupName: "<name>_backup", "<name>_backup1", ... picking the
// first name that does not already exist.
static std::filesystem::path generate_backup_name(const std::filesystem::path& dir) {
    auto backup = dir.string() + "_backup";
    if (!std::filesystem::exists(backup)) return backup;
    for (int i = 1;; ++i) {
        auto candidate = dir.string() + "_backup" + std::to_string(i);
        if (!std::filesystem::exists(candidate)) return candidate;
    }
}

bool InstallStage::execute(Mod& mod, PipelineContext& ctx) {
    // Find staging directory from the mod's files (set by ExtractStage)
    std::filesystem::path staging_dir;
    for (const auto& f : mod.files) {
        auto p = std::filesystem::path(f.relative_path);
        if (std::filesystem::exists(p / "metadata.xml") ||
            std::filesystem::is_directory(p)) {
            staging_dir = p;
            break;
        }
    }

    if (staging_dir.empty()) {
        // Metadata-only mod (e.g. Steam Workshop already on disk) - nothing to install
        if (mod.state == ModState::Extracted) {
            mod.state = ModState::Installed;
            return true;
        }
        Logger::instance().error("InstallStage: no staging directory found");
        return false;
    }

    // Determine mod folder name - the display name (e.g. "SkyUI") is the
    // MO2-style folder name. The download id (mod_id-file_id) is only a
    // fallback for sources that never resolved a display name.
    std::string folder_name = mod.name;
    if (folder_name.empty()) {
        folder_name = mod.id;
    }
    if (folder_name.empty()) {
        folder_name = mod.download_source_id;
    }
    if (folder_name.empty()) {
        Logger::instance().error("InstallStage: cannot determine mod folder name");
        return false;
    }

    // Sanitize folder name (replace problematic characters)
    for (auto& c : folder_name) {
        if (c == '/' || c == '\\' || c == '\0') c = '_';
    }

    // Destination in mods/
    auto mods_dir = ctx.mods_dir;
    if (mods_dir.empty() && ctx.instance) {
        mods_dir = ctx.instance->path_for(InstanceKind::Mods);
    }
    if (mods_dir.empty()) {
        Logger::instance().error("InstallStage: no mods directory in context");
        return false;
    }

    // Ask the user how to proceed when the mod folder already exists (MO2's
    // testOverwrite in installationmanager.cpp): Merge adds files into the
    // existing folder, Replace deletes it and installs fresh, Rename installs
    // under a new folder name (re-checked in a loop, since the new name may
    // also exist), Cancel aborts. Without a callback the headless default is a
    // silent replace (the behavior before the query dialog existed).
    auto dest_dir = mods_dir / folder_name;
    while (std::filesystem::exists(dest_dir)) {
        if (!ctx.overwrite_query_cb) {
            Logger::instance().warn("InstallStage: mod folder already exists, removing: " +
                                    dest_dir.string());
            std::error_code ec;
            std::filesystem::remove_all(dest_dir, ec);
            break;
        }

        auto decision = ctx.overwrite_query_cb(folder_name);
        if (decision.action == OverwriteAction::Cancel) {
            Logger::instance().debug("InstallStage: install canceled by user");
            return false;
        }

        if (decision.backup) {
            auto backup_dir = generate_backup_name(dest_dir);
            Logger::instance().debug("InstallStage: backing up " + dest_dir.string() +
                                     " to " + backup_dir.string());
            if (!copy_recursive(dest_dir, backup_dir)) {
                Logger::instance().error("InstallStage: failed to create backup " +
                                         backup_dir.string());
                return false;
            }
        }

        if (decision.action == OverwriteAction::Rename) {
            folder_name = decision.new_name;
            for (auto& c : folder_name) {
                if (c == '/' || c == '\\' || c == '\0') c = '_';
            }
            if (folder_name.empty()) {
                Logger::instance().error("InstallStage: rename produced an empty folder name");
                return false;
            }
            dest_dir = mods_dir / folder_name;
            continue;  // re-check: the new name may also exist
        }

        if (decision.action == OverwriteAction::Replace) {
            Logger::instance().warn("InstallStage: replacing existing mod folder " +
                                    dest_dir.string());
            std::error_code ec;
            std::filesystem::remove_all(dest_dir, ec);
        }

        // Merge (existing folder kept) or Replace (fresh empty folder) both
        // fall through to the copy below.
        break;
    }

    // Copy extracted files to mods/{folder_name}/
    Logger::instance().debug("InstallStage: installing to " + dest_dir.string());
    if (!copy_recursive(staging_dir, dest_dir)) {
        Logger::instance().error("InstallStage: failed to copy files to " + dest_dir.string());
        return false;
    }

    // Ensure the game's metadata file exists in the mod folder so ModScanner
    // can find it. MO2-style games get a meta.ini (MO2's installers write the
    // same file with the same keys); metadata.xml is an Isaac-only trick - the
    // Isaac engine reads it from mod folders directly - and is written only
    // for games that registered the metadata_file hook.
    std::string display_name = mod.name.empty() ? folder_name : mod.name;
    ModMeta::write_game_metadata(dest_dir, ctx.metadata_file, display_name,
                                 mod.version, mod.download_source_id,
                                 mod.archive_filename);

    // Clean up staging directory
    std::error_code ec;
    std::filesystem::remove_all(staging_dir, ec);

    // Write meta.ini
    auto meta_dir = ctx.meta_dir;
    if (meta_dir.empty() && ctx.instance) {
        meta_dir = ctx.instance->path_for(InstanceKind::Meta);
    }

    if (!meta_dir.empty()) {
        auto meta = ModMeta::from_default(
            folder_name,
            mod.download_source_type.empty() ? "manual" : mod.download_source_type,
            mod.download_source_id,
            mod.archive_filename,
            mod.version);

        // Reinstall: preserve the previously persisted priority so the mod keeps
        // its position in the load order instead of resetting to the top.
        auto existing = ModMeta::load(meta_dir, folder_name);
        if (existing.priority() >= 0) {
            meta.set_priority(existing.priority());
        }

        // For Nexus downloads, add [Nexusmods] section
        if (mod.download_source_type == "nexus" && mod.download_nxm.file_id > 0) {
            meta.set("Nexusmods", "modid", mod.download_source_id);
            meta.set("Nexusmods", "fileid", std::to_string(mod.download_nxm.file_id));
        }

        if (!meta.save(meta_dir, folder_name)) {
            Logger::instance().warn("InstallStage: failed to write meta.ini for " + folder_name);
        }
    }

    mod.id = folder_name;
    mod.state = ModState::Installed;
    Logger::instance().debug("InstallStage: installed " + folder_name);
    return true;
}

} // namespace engine
