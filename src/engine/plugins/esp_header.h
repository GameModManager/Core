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
};

// Read and parse the TES4 header of a .esm/.esp/.esl file.
// Unreadable / non-TES4 / truncated files yield valid=false (never throws).
EspHeaderInfo read_esp_header(const std::filesystem::path& file_path);

}  // namespace engine
