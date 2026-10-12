#pragma once

#include "engine/game/esp/plugin_info.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace engine {

class Platform;

namespace Sorter {
  namespace Loot {

    // One plugin fed to the LOOT sorter. `full_path` is the winning on-disk file
    // the game sees (mod-over-game resolution already happened in
    // PluginDatabase::refresh) - no VFS or merged Data dir is needed (PLAN.md
    // 7.1).
    struct Plugin {
      std::string name;  // filename, e.g. "SkyUI_SE.esp"
      std::filesystem::path full_path;
    };

    // Input for one "sort with LOOT" run (PLAN.md 7.1). The UI builds this from
    // a refreshed PluginDatabase and applies the result back onto its rows.
    struct Request {
      std::string game_id;             // GMM game_id, e.g. "SkyrimSpecialEdition"
      std::string loot_game_id;        // LOOT game slug, e.g. "skyrimse"
      std::string masterlist_repo;     // loot/<repo>, e.g. "skyrimse"
      std::filesystem::path game_dir;  // game install root
      std::filesystem::path
          profile_dir;                 // MO2-format profile (loadorder.txt/plugins.txt)
      std::filesystem::path cli_path;  // gmm_lootcli binary; empty = not built
      const Platform *platform = nullptr;  // data_dir() for masterlist cache
      std::vector<Plugin> plugins;  // every plugin (natives + CC + user), winning paths
      bool update_masterlists = true;  // refresh stale masterlists before sorting
    };

    // Run statistics MO2 reads back out of the report's "stats" object
    // (Loot::reportStats, loot.cpp:853-860): how long the sort took and which
    // lootcli / libloot produced it. `time` is milliseconds.
    struct Stats {
      int64_t time = 0;
      std::string lootcli_version;
      std::string loot_version;
    };

    struct Result {
      bool ok = false;
      std::string error;                      // user-facing failure reason
      std::vector<std::string> sorted_names;  // new order, top = most dominant
      std::filesystem::path report_path;      // CLI JSON report (debugging)
      std::vector<std::string> messages;      // [level] lines relayed from the CLI
      // Game-wide LOOT messages from the report's top-level "messages" array
      // (the ones MO2 shows in LootDialog::showReport above the plugin list).
      // `level` is the JSON "type": "info" | "warn" | "error".
      struct Message {
        std::string level;
        std::string text;
      };
      std::vector<Message> general_messages;
      Stats stats;  // report's "stats" object
      // Per-plugin LOOT findings parsed from the report's "plugins" array
      // (MO2 lootcli createPlugins shape), keyed by plugin filename. Empty
      // when the CLI wrote no findings. Mirrors LootDialog::showReport
      // building the per-plugin data for addLootReport (lootdialog.cpp:287-295).
      std::map<std::string, LootReport> reports;
    };

    // Progress stages mirror the lootcli Progress enum (MO2 lootcli.h):
    // 1 CheckingMasterlistExistence, 2 UpdatingMasterlist, 3 LoadingLists,
    // 4 ReadingPlugins, 5 SortingPlugins, 6 WritingLoadorder, 7 ParsingLootMessages,
    // 8 Done.
    using ProgressFn = std::function<void(int stage, const std::string &message)>;

    // Run gmm_lootcli against the request. On success Result::sorted_names is
    // the sorted order for every plugin (the caller applies it, preserving the
    // fixed native/CC band and locked plugins). On failure Result::error
    // carries a user-facing reason (CLI missing, masterlists unavailable, CLI
    // error). `progress` (optional) receives stage markers as they stream by.
    Result run_sort(const Request &request, ProgressFn progress = {});

  }  // namespace Loot
}  // namespace Sorter

}  // namespace engine
