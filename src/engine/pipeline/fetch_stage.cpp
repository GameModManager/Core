#include "engine/pipeline/fetch_stage.h"
#include "engine/pipeline/pipeline.h"
#include "engine/mod/model/mod.h"
#include "engine/source/source_provider.h"
#include "engine/core/instance/instance.h"
#include "engine/core/log/logger.h"

#include <sstream>

namespace engine {

bool FetchStage::execute(Mod& mod, PipelineContext& ctx) {
    // No download info → nothing to fetch
    if (mod.download_source_type.empty()) {
        mod.state = ModState::Downloaded;
        return true;
    }

    // Find provider for this source type
    auto* provider = SourceRegistry::instance().provider_for(mod.download_source_type);
    if (!provider) {
        Logger::instance().error("FetchStage: no provider for source type '" +
                                 mod.download_source_type + "'");
        return false;
    }

    // Determine download destination
    std::filesystem::path dest_dir;
    if (ctx.instance) {
        dest_dir = ctx.instance->path_for(InstanceKind::Downloads);
    } else {
        dest_dir = ctx.mods_dir.parent_path() / "downloads";
    }
    std::error_code ec;
    std::filesystem::create_directories(dest_dir, ec);

    // Build archive filename. Providers may resolve the real file name (e.g.
    // Nexus file_name, with the correct extension); otherwise fall back to the
    // generic "<source_id>[-<file_id>].zip".
    std::ostringstream fname;
    auto info = provider->resolve_download_info(mod);
    if (!info.archive_name.empty()) {
        fname << info.archive_name;
    } else {
        fname << mod.download_source_id;
        if (mod.download_nxm.file_id > 0)
            fname << "-" << mod.download_nxm.file_id;
        fname << ".zip";
    }
    mod.archive_filename = fname.str();

    // Carry the real display name (e.g. "SkyUI") up to the UI. Only when the
    // provider actually resolved one - the PipelineWorker's placeholder
    // ("Mod file <id>") is left untouched otherwise.
    if (!info.display_name.empty())
        mod.name = info.display_name;

    // Surface the resolved metadata (archive name + display name) as soon as
    // it is known - before the download starts - so the UI can replace its
    // placeholder row name immediately rather than at download_complete.
    if (ctx.on_download_meta)
        ctx.on_download_meta(info.archive_name, info.display_name);

    auto dest_path = dest_dir / mod.archive_filename;

    // Resume support: if a partial download already exists (a paused download
    // was aborted and kept its file), continue from its size via HTTP Range.
    ctx.download_resume_from = 0;
    if (std::filesystem::exists(dest_path, ec)) {
        auto sz = std::filesystem::file_size(dest_path, ec);
        if (!ec && sz > 0) {
            ctx.download_resume_from = static_cast<int64_t>(sz);
            Logger::instance().debug("FetchStage: resuming partial download of " +
                                     mod.download_source_id + " at byte " +
                                     std::to_string(ctx.download_resume_from));
        }
    }

    Logger::instance().debug("FetchStage: downloading " + mod.download_source_type +
                            " mod " + mod.download_source_id +
                            " to " + dest_path.string());

    if (!provider->fetch(mod, ctx, dest_path)) {
        Logger::instance().error("FetchStage: download failed");
        return false;
    }

    // Some providers (e.g. SteamWorkshop) are metadata-only - no file produced
    if (!std::filesystem::exists(dest_path)) {
        mod.archive_filename.clear();
        mod.state = ModState::Downloaded;
        Logger::instance().debug("FetchStage: metadata updated, no archive file");
        return true;
    }

    // Add archive to mod files for subsequent stages
    ModFile mf;
    mf.relative_path = dest_path.string();
    mod.files.push_back(mf);

    mod.state = ModState::Downloaded;
    Logger::instance().debug("FetchStage: download complete");
    return true;
}

} // namespace engine
