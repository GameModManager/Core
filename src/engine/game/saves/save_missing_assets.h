#pragma once

#include "engine/game/plugins/plugin_info.h"
#include "engine/game/saves/save_game.h"

#include <filesystem>
#include <string>
#include <unordered_map>
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

// Finds save plugins the current load order can't satisfy. For a 106-save
// scan, the underlying mods-dir walk in pass 2 was repeated 106 times. Use
// build_save_provider_index once and call the indexed overload to share that
// work across every save.
std::vector<SaveMissingAsset> find_save_missing_assets(
    const SaveGame& save, const std::vector<GamePlugin>& plugins,
    const std::filesystem::path& mods_dir,
    const std::filesystem::path& overwrite_dir = {});

// lowercased plugin name -> list of providing mod names. "<overwrite>" is the
// sentinel for the overwrite dir. Built once per scan and shared across saves.
using SaveProviderIndex = std::unordered_map<std::string, std::vector<std::string>>;

// One-shot provider lookup. Walks mods_dir + overwrite_dir, returns a map keyed
// by lowercased plugin filename.
[[nodiscard]] SaveProviderIndex build_save_provider_index(
    const std::filesystem::path& mods_dir,
    const std::filesystem::path& overwrite_dir = {});

// Indexed variant: pass 1 logic is identical, pass 2 reads prebuilt index
// instead of re-walking the mods/overwrite dirs.
std::vector<SaveMissingAsset> find_save_missing_assets(
    const SaveGame& save, const std::vector<GamePlugin>& plugins,
    const SaveProviderIndex& provider_index);

}  // namespace engine
