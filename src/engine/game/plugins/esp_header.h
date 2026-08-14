#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace engine {

// Parsed TES4 record header for a Bethesda plugin file.
struct EspHeaderInfo {
    bool valid = false;
    bool is_master = false;   // record header flag bit 0 (ESM)
    bool is_light = false;    // bit 9 (ESL) - Skyrim SE / Fallout 4
    bool is_medium = false;   // bit 10 (ESH) - Starfield
    bool localized = false;   // bit 7
    std::vector<std::string> masters;  // MAST subrecords in file order

    // Form version from the record header version stamp (bytes 20-24),
    // matching MO2's ESP::File::formVersion() (44 for Skyrim SE, 43 for LE).
    uint32_t form_version = 0;
    // Header version from the HEDR subrecord float (0.94, 1.70, 1.74, ...).
    float header_version = 0.0f;
    // Number of records from the HEDR subrecord (0 => dummy plugin).
    uint32_t num_records = 0;
    std::string author;       // CNAM subrecord
    std::string description;  // SNAM subrecord
};

// Read and parse the TES4 header of a .esm/.esp/.esl file.
// Unreadable / non-TES4 / truncated files yield valid=false (never throws).
EspHeaderInfo read_esp_header(const std::filesystem::path& file_path);

}  // namespace engine
