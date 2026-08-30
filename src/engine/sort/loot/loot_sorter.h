#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace engine {

class Platform;

// One plugin fed to the LOOT sorter. `full_path` is the winning on-disk file
// the game sees (mod-over-game resolution already happened in
// PluginDatabase::refresh) - no VFS or merged Data dir is needed (PLAN.md
// §7.1).
struct LootPlugin {
    std::string name;               // filename, e.g. "SkyUI_SE.esp"
    std::filesystem::path full_path;
};

// Input for one "sort with LOOT" run (PLAN.md §7.1). The UI builds this from
// a refreshed PluginDatabase and applies the result back onto its rows.
struct LootRequest {
    std::string game_id;            // GMM game_id, e.g. "SkyrimSpecialEdition"
    std::string loot_game_id;       // LOOT game slug, e.g. "skyrimse"
    std::string masterlist_repo;    // loot/<repo>, e.g. "skyrimse"
    std::filesystem::path game_dir;    // game install root
    std::filesystem::path profile_dir; // MO2-format profile (loadorder.txt/plugins.txt)
    std::filesystem::path cli_path;    // gmm_lootcli binary; empty = not built
    const Platform* platform = nullptr;  // data_dir() for masterlist cache
    std::vector<LootPlugin> plugins;   // every plugin (natives + CC + user), winning paths
    bool update_masterlists = true;    // refresh stale masterlists before sorting
};

struct LootResult {
    bool ok = false;
    std::string error;                  // user-facing failure reason
    std::vector<std::string> sorted_names;  // new order, top = most dominant
    std::filesystem::path report_path;      // CLI JSON report (debugging)
    std::vector<std::string> messages;      // [level] lines relayed from the CLI
};

// Progress stages mirror the lootcli Progress enum (MO2 lootcli.h):
// 1 CheckingMasterlistExistence, 2 UpdatingMasterlist, 3 LoadingLists,
// 4 ReadingPlugins, 5 SortingPlugins, 6 WritingLoadorder, 7 ParsingLootMessages,
// 8 Done.
using LootProgressFn = std::function<void(int stage, const std::string& message)>;

// Run gmm_lootcli against the request. On success LootResult::sorted_names is
// the sorted order for every plugin (the caller applies it, preserving the
// fixed native/CC band and locked plugins). On failure LootResult::error
// carries a user-facing reason (CLI missing, masterlists unavailable, CLI
// error). `progress` (optional) receives stage markers as they stream by.
LootResult run_loot_sort(const LootRequest& request,
                         LootProgressFn progress = {});

}  // namespace engine
