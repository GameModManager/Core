#include "engine/pipeline/install_stage.h"
#include "engine/pipeline/pipeline.h"
#include "engine/model/mod.h"
#include "engine/instance/instance.h"
#include "engine/meta/mod_meta.h"
#include "engine/log/logger.h"

#include <fstream>

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
        // Metadata-only mod (e.g. Steam Workshop already on disk) — nothing to install
        if (mod.state == ModState::Extracted) {
            mod.state = ModState::Installed;
            return true;
        }
        Logger::instance().error("InstallStage: no staging directory found");
        return false;
    }

    // Determine mod folder name
    std::string folder_name = mod.id;
    if (folder_name.empty()) {
        folder_name = mod.name;
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

    auto dest_dir = mods_dir / folder_name;

    // Check if mod already exists
    if (std::filesystem::exists(dest_dir)) {
        Logger::instance().warn("InstallStage: mod folder already exists, removing: " +
                                dest_dir.string());
        std::error_code ec;
        std::filesystem::remove_all(dest_dir, ec);
    }

    // Copy extracted files to mods/{folder_name}/
    Logger::instance().debug("InstallStage: installing to " + dest_dir.string());
    if (!copy_recursive(staging_dir, dest_dir)) {
        Logger::instance().error("InstallStage: failed to copy files to " + dest_dir.string());
        return false;
    }

    // Ensure a metadata.xml exists in the mod folder so ModScanner can find it
    auto metadata_path = dest_dir / "metadata.xml";
    if (!std::filesystem::exists(metadata_path)) {
        std::string display_name = mod.name;
        if (display_name.empty()) display_name = folder_name;
        std::string ver = mod.version.empty() ? "1.0" : mod.version;
        std::ofstream mf(metadata_path);
        if (mf) {
            mf << "<?xml version=\"1.0\"?>\n"
               << "<mod>\n"
               << "  <name>" << display_name << "</name>\n"
               << "  <version>" << ver << "</version>\n"
               << "</mod>\n";
        }
    }

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
