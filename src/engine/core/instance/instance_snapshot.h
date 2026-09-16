#pragma once

// Comprehensive per-instance state snapshot. Aggregates ALL tracked state
// into a single serializable object. This IS what modpacks export and
// what modpack install restores to.
//
// Captures:
//   - Instance config (game_id, display_name, deploy_strategy, etc.)
//   - All mod/separator tracking entries (ModTrackingEntry)
//   - All profile state (modlist, plugins, load order, archives, settings, ini tweaks)
//   - Executables (from instance.toml [[executables]])
//
// Engine layer, Qt-free, JSON serializable (nlohmann/json).

#include "engine/core/instance/instance.h"
#include "engine/core/instance/mod_state.h"
#include "engine/profile/profile.h"

#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine {

// Executable entry matching instance.toml [[executables]].
struct ExecutableEntry {
  std::string path;
  std::string title;
  std::string args;
  std::string cwd;
  std::string mod;
  std::string icon;
  std::vector<std::string> env;
};

// One deployed-file manifest entry: game-relative target -> absolute source.
// Mirrors one row of the <instance>/.gmm_deploy_ledger TSV (target\t source).
struct DeployedFile {
  std::string target;
  std::string source;
};

// Snapshot of one profile's state.
struct ProfileSnapshot {  std::string name;
  std::vector<profile::ModListEntry> mods; // priority-sorted ascending
  std::vector<std::string> plugins;
  std::vector<std::string> load_order;
  std::vector<profile::LockedPlugin> locked_order;
  std::vector<std::string> archives;
  bool local_saves = false;
  bool local_settings = false;
  bool auto_archive_invalidation = false;
  std::string tweaked_ini;
};

// Comprehensive instance state snapshot.
struct InstanceSnapshot {
  // Instance config (non-path fields from instance.toml).
  std::string game_id;
  std::string display_name;
  bool portable = true;
  std::string deploy_strategy;
  std::string proton_runner;
  uint32_t steam_appid = 0;

  // All mod/separator tracking entries.
  std::unordered_map<std::string, ModTrackingEntry> mod_entries;
  uint32_t next_install_order = 0;

  // Profile state (one per profile directory).
  std::vector<ProfileSnapshot> profiles;

  // Executables from instance.toml [[executables]].
  std::vector<ExecutableEntry> executables;

  // Deployed file manifest (<instance>/.gmm_deploy_ledger TSV rows).
  std::vector<DeployedFile> deployed_files;

  // Capture all tracked state from an instance on disk.
  [[nodiscard]] static InstanceSnapshot capture(const Instance& instance);

  // Apply snapshot state to an instance on disk.
  // Writes instance.toml, mod_state.json, and all profile files.
  // Missing profile directories are created; existing ones are overwritten.
  [[nodiscard]] bool apply(const Instance& instance) const;

  // JSON serialization.
  [[nodiscard]] nlohmann::json to_json() const;
  [[nodiscard]] static InstanceSnapshot from_json(const nlohmann::json& j);

  // File I/O (convenience wrappers).
  [[nodiscard]] bool save(const std::filesystem::path& path) const;
  [[nodiscard]] static InstanceSnapshot load(const std::filesystem::path& path);
};

} // namespace engine
