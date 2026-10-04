#pragma once

// Port of MO2's GamebryoModDataChecker
// (REFERENCES/modorganizer-game_bethesda/src/gamebryo/gamebryomoddatachecker.cpp):
// decides whether a file tree already looks like a game's Data folder. This
// static utility is the engine's DEFAULT: the staging-layout decision asks the
// declaring game instead whenever it has a mod_data_checker feature or
// mod_valid_dirs / mod_valid_exts (ModDataCheckerFeature, resolved by
// data_checker_for()), and only falls back here when the game declares
// nothing. Qt-free.

#include <memory>
#include <string>
#include <unordered_set>

#include "engine/mod/filetree/file_tree.h"

namespace engine {

class ModDataChecker {
public:
  // True when the tree contains at least one known data-dir folder or a file
  // with a known data-dir extension, matched case-insensitively (MO2
  // dataLooksValid() returning CheckReturn::VALID).
  static bool data_looks_valid(const std::shared_ptr<const FileTree> &tree);

  // MO2 possibleFolderNames() (Gamebryo set, lowercased - matched CI),
  // plus "source" (a real Skyrim Data folder carries the CK sources).
  static const std::unordered_set<std::string> &folder_names();

  // MO2 possibleFileExtensions().
  static const std::unordered_set<std::string> &file_extensions();
};

}  // namespace engine
