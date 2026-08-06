#pragma once

// P1.2 GameFeatureRegistry — MO2's IGameFeatures analogue.
//
// MO2's real per-game extension mechanism is nine behavior classes
// (ModDataChecker, ModDataContent, DataArchives, ScriptExtender,
// SaveGameInfo, LocalSavegames, UnmanagedMods, BSAInvalidation, GamePlugins)
// that any plugin can register/override per game with priority + replace
// (REFERENCES/modorganizer/src/game_features.{h,cpp}). GameModManager folds
// all nine into hardcoded engine hooks + per-game plugin classes; this module
// is the registry seam that lets a plugin add or override one. The game
// plugin's own feature registers at the LOWEST priority (MO2 model: the
// game's built-in feature is the baseline everything else overrides).
//
// ModDataChecker is the first concrete feature (already had
// content_looks_valid/mod_valid_dirs as its body — extraction, not net-new
// work). Qt-free.

#include <memory>
#include <string>
#include <vector>

#include "engine/filetree/file_tree.h"

namespace engine {

// Base for any per-game behavior a plugin can register or override. The
// registry keys features by type_name(); concrete subclasses carry the
// feature-specific data.
class GameFeature {
public:
    virtual ~GameFeature() = default;

    // Registry key for this feature class, e.g. "mod_data_checker".
    virtual const char* type_name() const = 0;
};

// Port of MO2's ModDataChecker (GamebryoModDataChecker): decides whether a
// file tree already looks like a game's Data folder. Instance-based — the
// allow-sets come from whoever registered the feature (the game plugin, or an
// overriding plugin), so the same class serves the base checker and the
// override. The engine's own static utility (ModDataChecker) stays untouched
// for the staging peel.
class ModDataCheckerFeature : public GameFeature {
public:
    // folders/extensions: top-level directory names / file extensions that
    // count as real game data (MO2 possibleFolderNames()/possibleFileExtensions()).
    ModDataCheckerFeature(std::vector<std::string> folders,
                          std::vector<std::string> extensions)
        : folders_(std::move(folders)), extensions_(std::move(extensions)) {}

    const char* type_name() const override { return "mod_data_checker"; }

    const std::vector<std::string>& folder_names() const { return folders_; }
    const std::vector<std::string>& file_extensions() const { return extensions_; }

    // MO2 dataLooksValid() returning VALID: the tree holds a top-level
    // directory named in folder_names() or a top-level file whose extension
    // is in file_extensions(), matched case-insensitively.
    bool data_looks_valid(const std::shared_ptr<const FileTree>& tree) const;

private:
    std::vector<std::string> folders_;
    std::vector<std::string> extensions_;
};

}  // namespace engine
