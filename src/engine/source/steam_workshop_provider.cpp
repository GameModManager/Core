#include "engine/source/steam_workshop_provider.h"
#include "engine/model/mod.h"
#include "engine/pipeline/pipeline.h"
#include "engine/meta/mod_meta.h"
#include "engine/log/logger.h"

namespace engine {

SteamWorkshopProvider::SteamWorkshopProvider(const std::string& db_path)
    : db_path_(db_path) {}

bool SteamWorkshopProvider::fetch(const Mod& mod, const PipelineContext& ctx,
                                   const std::filesystem::path& dest_path) {
    (void)dest_path;
    if (mod.download_source_type != "steam") return false;

    // Parse workshop_id from download_source_id
    int64_t workshop_id = 0;
    try {
        workshop_id = std::stoll(mod.download_source_id);
    } catch (...) {
        Logger::instance().error("SteamWorkshopProvider: invalid workshop_id: " +
                                 mod.download_source_id);
        return false;
    }

    // Lazy-init WorkshopClient
    if (!client_) {
        client_ = std::make_unique<WorkshopClient>(db_path_);
    }

    // Fetch metadata from Steam API (uses SQLite cache, respects rate limits)
    auto item = client_->get_details(workshop_id);
    if (!item) {
        Logger::instance().warn("SteamWorkshopProvider: no metadata for workshop_id=" +
                                std::to_string(workshop_id) +
                                " (cached, dead, or rate-limited)");
        // Return true anyway - the mod is already on disk, metadata is optional
        return true;
    }

    // Write metadata to meta.ini
    auto meta_dir = ctx.meta_dir;
    if (meta_dir.empty()) {
        Logger::instance().warn("SteamWorkshopProvider: no meta_dir in context");
        return true;
    }

    std::string folder_name = mod.id;
    if (folder_name.empty()) {
        folder_name = mod.download_source_id;
    }
    if (folder_name.empty()) return true;

    // Load existing meta or start fresh
    auto meta = ModMeta::load(meta_dir, folder_name);
    meta.set("SteamWorkshop", "title", item->title);
    meta.set("SteamWorkshop", "preview_url", item->preview_url);
    meta.set("SteamWorkshop", "description", item->description);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.0f", item->created_at);
    meta.set("SteamWorkshop", "created_at", buf);
    std::snprintf(buf, sizeof(buf), "%.0f", item->updated_at);
    meta.set("SteamWorkshop", "updated_at", buf);
    meta.set("SteamWorkshop", "status", item->status);
    meta.set("GameModManager", "source_type", "steam");
    meta.set("GameModManager", "source_id", std::to_string(workshop_id));

    if (!meta.save(meta_dir, folder_name)) {
        Logger::instance().warn("SteamWorkshopProvider: failed to save meta.ini");
    }

    Logger::instance().debug("SteamWorkshopProvider: updated metadata for workshop " +
                            std::to_string(workshop_id));
    return true;
}

} // namespace engine
