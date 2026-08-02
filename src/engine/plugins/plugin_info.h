#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace engine {

// A single plugin (.esm/.esp/.esl) discovered in the game's merged Data view.
// Mirrors the fields of MO2's ESPInfo (modorganizer/src/pluginlist.h) that the
// engine needs: load order, enable state, masters, flags, and mod index.
struct GamePlugin {
    std::string name;               // filename, e.g. "SkyUI_SE.esp"
    std::string owner_mod;          // mod folder providing this plugin ("" = game Data)
    std::filesystem::path full_path;  // on-disk source file
    std::vector<std::string> masters;

    bool is_master_flagged = false;  // ESM header flag (bit 0)
    bool is_light_flagged = false;   // ESL header flag (bit 9) - Skyrim SE / Fallout 4
    bool is_medium_flagged = false;  // ESH header flag (bit 10) - Starfield
    bool has_master_ext = false;     // .esm extension
    bool has_light_ext = false;      // .esl extension
    bool is_game_native = false;    // vanilla ESM that ships with the game
    bool is_cc = false;             // listed in Skyrim.ccc - the game force-loads it
    bool force_loaded = false;      // always enabled, cannot be toggled
    bool enabled = false;
    bool missing_master = false;    // a required master is absent from the list

    // Type checks: header flag OR extension, matching the flag meaning that
    // older GMM versions collapsed into a single bool (.esh has no extension
    // field — it maps straight to is_medium_flagged).
    [[nodiscard]] bool is_master() const { return is_master_flagged || has_master_ext; }
    [[nodiscard]] bool is_light() const { return is_light_flagged || has_light_ext; }
    [[nodiscard]] bool is_medium() const { return is_medium_flagged; }

    int priority = -1;              // position in the plugin list (0 = top = most dominant)
    int mod_priority = -1;          // priority of the owning mod (ordering tiebreak)

    // FormID prefix shown in the Mod Index column. For regular plugins: 0..0xFF.
    // For light plugins (ESL): 0xFE000000 | ordinal. For medium (ESH): 0xFD000000 | ordinal.
    uint32_t mod_index = 0;
    std::string mod_index_text;     // display form: "00", "FE:001", ...

    [[nodiscard]] bool valid() const { return !name.empty(); }
};

}  // namespace engine
