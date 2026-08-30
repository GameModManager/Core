#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace engine {

enum class ModType {
    Regular,      // standard managed mod
    Foreign,      // DLC/CC: game-native
    Separator,    // separator row
    Backup,       // backup mod
    Overwrite,    // overwrite pseudo-row
    Merged,       // MERGED pseudo-row
    GameNative,   // unmanaged game plugin
};

enum class ModState {
    Downloaded,
    Extracted,
    Installed,
    Staged,
    Deployed,
};

struct ModFile {
    std::string relative_path;
    uint64_t size = 0;
};

struct Mod {
    std::string id;
    std::string name;
    std::string version;
    ModType type = ModType::Regular;
    ModState state = ModState::Downloaded;
    std::vector<ModFile> files;

    // Download metadata (populated before pipeline run for remote sources)
    std::string download_source_type;  // "nexus", "steam", "manual"
    std::string download_source_id;    // Nexus mod_id or Steam workshop_id
    struct {
        int64_t file_id = 0;
        std::string key;
        int64_t expire = 0;
        int64_t user_id = 0;
        std::string nexus_domain;  // e.g. "skyrimspecialedition"
    } download_nxm;

    // Pre-resolved direct download URL. When set, providers download from this
    // URL directly instead of resolving one themselves.
    std::string download_url;

    // Source page URL for the download (e.g. a LoversLab file page). Persisted
    // into the mod's per-source meta section so the UI can link back to it.
    std::string download_page_url;

    // Archive filename determined during fetch (e.g. "mod-12345-1-0.zip")
    std::string archive_filename;
};

}  // namespace engine
