#pragma once

// Port of MO2's GamebryoModDataChecker
// (REFERENCES/modorganizer-game_bethesda/src/gamebryo/gamebryomoddatachecker.cpp):
// decides whether a file tree already looks like a game's Data folder. This is
// the P1.2 Game::Features::Registry seed - a static utility today, per-game
// override hooks (and the feature-registry lookup) arrive with that phase.
// Qt-free.

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

} // namespace engine
