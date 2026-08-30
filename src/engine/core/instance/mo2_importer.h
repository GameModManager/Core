#pragma once

#include "engine/core/instance/instance.h"
#include "engine/game/detect/game_detector.h"

#include <filesystem>
#include <map>
#include <string>

namespace engine {

// MO2 game name -> GMM game_id mapping table
struct Mo2GameMap {
  // Look up a game_id from MO2's game name. Returns empty string if not found.
  static std::string lookup(const std::string &mo2_game_name);

  // Fuzzy match: tries exact, then case-insensitive, then substring
  static std::string fuzzy_lookup(const std::string &mo2_game_name);
};

// MO2 import result (extends InstanceManager::ImportResult with MO2-specific
// data)
struct Mo2ImportResult {
  bool success = false;
  std::string error;
  Instance instance;
  int mods_imported = 0;
  int profiles_imported = 0;
  std::string mo2_game_name;
  std::string mapped_game_id;
};

// Import an MO2 instance directory into GMM
Mo2ImportResult import_mo2_instance(const std::filesystem::path &mo2_dir,
                                    const std::filesystem::path &gmm_root,
                                    const std::string &display_name = "");

} // namespace engine
