#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace engine {

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

    // Archive filename determined during fetch (e.g. "mod-12345-1-0.zip")
    std::string archive_filename;
};

}  // namespace engine
