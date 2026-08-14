#pragma once

// Qt-free resolver for the Saves tab's "missing assets" column. Port of MO2
// GamebryoSaveGameInfo::getMissingAssets
// (REFERENCES/modorganizer-game_bethesda/src/gamebryo/gamebryosavegameinfo.cpp):
// a plugin referenced by a save is a missing asset when the current load order
// doesn't have it *enabled* (MO2 STATE_INACTIVE / STATE_MISSING). For each such
// plugin we report the mod that owns it in the load order (when present but
// disabled) plus every installed mod whose top level holds a same-named plugin
// file (so the user can see it's installed-but-off, needs re-installing, or is
// genuinely gone).

#include "engine/game/plugins/plugin_info.h"
#include "engine/game/saves/save_game.h"

#include <filesystem>
#include <string>
#include <vector>

namespace engine {

struct SaveMissingAsset {
    std::string plugin_name;
    std::string origin_mod;                  // load-order owner (empty when the
                                             // plugin is absent from the list)
    bool inactive = false;                   // present in the load order but disabled
    std::vector<std::string> providing_mods;  // mods whose top level holds a
                                             // same-named .esp/.esl/.esm file
};

// Finds save plugins the current load order can't satisfy.
//   plugins      — PluginDatabase::plugins(); the `enabled` flag decides
//                  active vs inactive (MO2 STATE_ACTIVE/INACTIVE).
//   mods_dir     — instance mods dir; every subdirectory is one mod, searched
//                  at its top level only (matches MO2's entryList(espFilter)
//                  and our install layout, where installers peel the Data/).
//   overwrite_dir — instance overwrite dir; a same-named plugin file there
//                  contributes the "<overwrite>" candidate like MO2. May be
//                  empty to skip the overwrite scan.
// Plugin names are matched case-insensitively (Windows filesystems), the same
// convention PluginDatabase uses for its master lookups.
std::vector<SaveMissingAsset> find_save_missing_assets(
    const SaveGame& save, const std::vector<GamePlugin>& plugins,
    const std::filesystem::path& mods_dir,
    const std::filesystem::path& overwrite_dir = {});

}  // namespace engine
