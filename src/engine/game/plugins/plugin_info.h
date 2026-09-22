#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace engine {

// LOOT per-plugin report (MO2 Loot::Plugin parity, references/modorganizer
// src/loot.h:49-60). Populated after a LOOT sort (see LootDialog::showReport
// -> addLootReport, lootdialog.cpp:287-295) and rendered as the bullet
// section of the plugin tooltip (PluginList::makeLootTooltip).
struct LootReport {
  // (name, displayName) pairs; displayName may be empty (falls back to name).
  std::vector<std::pair<std::string, std::string>> incompatibilities;
  std::vector<std::string> missing_masters;
  struct Message {
    std::string level;  // "warning" | "error" | anything else (no prefix)
    std::string text;
  };
  std::vector<Message> messages;
  struct DirtyEntry {
    long long itm_records        = 0;
    long long deleted_references = 0;
    long long deleted_navmeshes  = 0;
    std::string cleaning_utility;  // empty renders as "?"
    std::string info;              // appended after a space when non-empty
  };
  std::vector<DirtyEntry> dirty;
  struct CleanEntry {
    std::string cleaning_utility;  // empty renders as "?"
  };
  std::vector<CleanEntry> clean;

  [[nodiscard]] bool empty() const {
    return incompatibilities.empty() && missing_masters.empty() && messages.empty() &&
           dirty.empty() && clean.empty();
  }
};

// A single plugin (.esm/.esp/.esl) discovered in the game's merged Data view.
// Mirrors the fields of MO2's ESPInfo (modorganizer/src/pluginlist.h) that the
// engine needs: load order, enable state, masters, flags, and mod index.
struct GamePlugin {
  std::string name;                 // filename, e.g. "SkyUI_SE.esp"
  std::string owner_mod;            // mod folder providing this plugin ("" = game Data)
  std::filesystem::path full_path;  // on-disk source file
  std::vector<std::string> masters;

  bool is_master_flagged = false;  // ESM header flag (bit 0)
  bool is_light_flagged  = false;  // ESL header flag (bit 9) - Skyrim SE / Fallout 4
  bool is_medium_flagged = false;  // ESH header flag (bit 10) - Starfield
  bool has_master_ext    = false;  // .esm extension
  bool has_light_ext     = false;  // .esl extension
  bool is_game_native    = false;  // vanilla ESM that ships with the game
  bool is_cc             = false;  // listed in Skyrim.ccc - the game force-loads it
  bool force_loaded      = false;  // always enabled, cannot be toggled
  bool force_enabled  = false;  // MO2 forceEnabled: can't be disabled (game-enforced)
  bool force_disabled = false;  // MO2 forceDisabled: the game can't load it (.esl
                                // without light support, None load mechanism)
  bool locked         = false;  // user-pinned: cannot be moved (auto-sort or drag)
  bool enabled        = false;
  bool missing_master = false;  // a required master is absent from the list
  std::vector<std::string> missing_masters;  // names of the absent masters
  // MO2 testMasters semantics (pluginlist.cpp:1342-1361): for ENABLED plugins
  // only, masters absent from the list OR present-but-disabled. Drives the
  // "Missing Masters" tooltip line and the warning emblem. Case-insensitive
  // sorted, deduplicated.
  std::vector<std::string> master_unset;

  // TES4 header metadata (MO2 tooltip parity). Unparsed/zero when the file
  // has no such record (form_version 0 is hidden, like MO2).
  uint32_t form_version = 0;
  float header_version  = 0.0f;
  bool has_no_records   = false;  // HEDR record count == 0 (dummy plugin)
  std::string author;             // CNAM
  std::string description;        // SNAM

  // Same-origin assets MO2 associates with the plugin (basename matching).
  bool has_ini = false;               // <basename>.ini in the owning folder
  std::vector<std::string> archives;  // <basename>* .bsa/.ba2 in the folder

  // Messages injected by registered diagnostics providers (tooltip <hr><ul>).
  std::vector<std::string> messages;

  // LOOT per-plugin report (incompatibilities, LOOT messages, dirty/clean
  // findings, missing masters). Empty until a LOOT sort populates it -
  // mirrors LootDialog::showReport -> addLootReport (lootdialog.cpp:287-295).
  LootReport loot_report;

  // Type checks: header flag OR extension, matching the flag meaning that
  // older GMM versions collapsed into a single bool (.esh has no extension
  // field - it maps straight to is_medium_flagged).
  [[nodiscard]] bool is_master() const { return is_master_flagged || has_master_ext; }
  [[nodiscard]] bool is_light() const { return is_light_flagged || has_light_ext; }
  [[nodiscard]] bool is_medium() const { return is_medium_flagged; }

  int priority     = -1;  // position in the plugin list (0 = top = most dominant)
  int mod_priority = -1;  // priority of the owning mod (ordering tiebreak)

  // FormID prefix shown in the Mod Index column. For regular plugins: 0..0xFF.
  // For light plugins (ESL): 0xFE000000 | ordinal. For medium (ESH): 0xFD000000 |
  // ordinal.
  uint32_t mod_index = 0;
  std::string mod_index_text;  // display form: "00", "FE:001", ...

  [[nodiscard]] bool valid() const { return !name.empty(); }
};

}  // namespace engine
