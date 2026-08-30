#include "engine/source/steam/provider.h"
#include "engine/mod/model/mod.h"
#include "engine/pipeline/pipeline.h"
#include "engine/mod/meta/mod_meta.h"
#include "engine/core/log/logger.h"

#include <cstdio>
#include <sstream>

namespace engine::Source::Steam {

Provider::Provider(const std::string& db_path,
                   int rate_limit, int rate_window)
    : db_path_(db_path), rate_limit_(rate_limit), rate_window_(rate_window) {}

bool Provider::fetch(const Mod& mod, PipelineContext& ctx,
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
        client_ = std::make_unique<WorkshopClient>(db_path_, rate_limit_, rate_window_);
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
    // Store tags as comma-separated list for category auto-assignment
    if (!item->tags.empty()) {
        std::string tags_csv;
        for (size_t i = 0; i < item->tags.size(); ++i) {
            if (i > 0) tags_csv += ',';
            tags_csv += item->tags[i];
        }
        meta.set("SteamWorkshop", "tags", tags_csv);
    }
    meta.set("GameModManager", "source_type", "steam");
    meta.set("GameModManager", "source_id", std::to_string(workshop_id));

    if (!meta.save(meta_dir, folder_name)) {
        Logger::instance().warn("SteamWorkshopProvider: failed to save meta.ini");
    }

    Logger::instance().debug("SteamWorkshopProvider: updated metadata for workshop " +
                            std::to_string(workshop_id));
    return true;
}

std::string Provider::display_name() const {
    return "Steam Workshop";
}

void Provider::set_rate_limit(int limit, int window) {
    rate_limit_ = limit;
    rate_window_ = window;
    if (client_) client_->set_rate_limit(limit, window);
}

} // namespace engine::Source::Steam
