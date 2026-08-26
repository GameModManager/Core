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
// All nine types are now registered features (0.2.26): ModDataChecker (the
// first extraction — content_looks_valid/mod_valid_dirs was its body),
// GamePlugins (the game_native_plugins hook's body), plus the seven
// structured-data features (mod_data_content, data_archives, script_extender,
// save_game_info, local_savegames, unmanaged_mods, bsa_invalidation) that
// enter through register_game_feature_data (key/value payload) and whose
// consumers arrive with their P2 phases (Saves, BSA/BA2, script extender).
// Qt-free.

#include <algorithm>
#include <filesystem>
#include <functional>

#include "engine/game/saves/save_game.h"
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "engine/mod/filetree/file_tree.h"

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

    static constexpr const char* type_key() { return "mod_data_checker"; }
    const char* type_name() const override { return type_key(); }

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

// Port of MO2's GamePlugins::gamePlugins(): the plugin files the game ships
// with and always enables (Skyrim's vanilla ESMs + _ResourcePack.esl). These
// appear as unmanaged mods in the list (cannot be removed/reordered) and head
// the fixed top band of the mod list / load order. Instance-based — the list
// comes from whoever registered the feature (the game plugin, or an overriding
// plugin).
class GamePluginsFeature : public GameFeature {
public:
    explicit GamePluginsFeature(std::vector<std::string> plugins)
        : plugins_(std::move(plugins)) {}

    static constexpr const char* type_key() { return "game_plugins"; }
    const char* type_name() const override { return type_key(); }

    const std::vector<std::string>& plugins() const { return plugins_; }

private:
    std::vector<std::string> plugins_;
};

// Port of MO2's ModDataContent (GamebryoModDataContent): the content
// categories a mod can hold, shown in the mod list's content column/badges.
// The standard Bethesda catalog is the shared engine-side default — identical
// across all Bethesda games, exactly like MO2 keeps it in the shared gamebryo
// code — and the registered feature carries which categories are enabled for
// this game (Skyrim SE disables SKYPROC) plus any name/icon overrides a plugin
// adds. contents_for() is the engine's generic classifier (a data-driven port
// of MO2's gamebryomoddatacontent.cpp), so changing how a game's mods are
// categorized never needs an engine change.
class ModDataContentFeature : public GameFeature {
public:
    struct Content {
        int id = 0;
        std::string name;
        std::string icon;  // icon key; empty = none
        bool filter_only = false;
    };

    ModDataContentFeature(std::vector<int> enabled_ids,
                          std::vector<Content> custom_contents = {})
        : enabled_ids_(std::move(enabled_ids)),
          custom_contents_(std::move(custom_contents)) {}

    static constexpr const char* type_key() { return "mod_data_content"; }
    const char* type_name() const override { return type_key(); }

    const std::vector<int>& enabled_ids() const { return enabled_ids_; }
    const std::vector<Content>& custom_contents() const { return custom_contents_; }

    // Standard catalog + custom overrides, filtered to the enabled set.
    std::vector<Content> all_contents() const;

    // Content IDs present in `tree` (MO2 getContentsFor). The script
    // extender's plugin path (resolved from the script_extender feature)
    // enables the SKSE / SKSE-FILES categories; pass empty to skip them.
    std::vector<int> contents_for(
        const std::shared_ptr<const FileTree>& tree,
        const std::string& script_extender_plugin_path = "") const;

private:
    std::vector<int> enabled_ids_;
    std::vector<Content> custom_contents_;
};

// Stable IDs for the standard Bethesda (gamebryo) content catalog. MO2's
// GamebryoModDataContent defines these as enum values + one shared list; here
// the same catalog is the engine-side default and the registered feature only
// toggles which IDs are enabled (mod_content_id_from_string maps the ABI's
// string IDs).
struct ModContentId {
    enum Value {
        Plugin = 0,
        Optional,
        Interface,
        Mesh,
        Bsa,
        Script,
        Skse,
        SkseFiles,
        Skyproc,
        Sound,
        Texture,
        Mcm,
        Ini,
        Facegen,
        Modgroup,
        Count,
        NextCustom = Count,
    };
};

// The shared Bethesda content catalog (name/icon/filter_only per standard ID).
[[nodiscard]] std::vector<ModDataContentFeature::Content> mod_content_catalog();

// "plugin" <-> ModContentId; -1 / "" when unknown.
[[nodiscard]] int mod_content_id_from_string(const std::string& id);
[[nodiscard]] std::string mod_content_string_from_id(int id);

// Port of MO2's DataArchives: the archive files the game loads itself (e.g.
// Skyrim - Textures*.bsa). The archives-tab reader/writer and the BSA
// invalidation wiring that consume this land in P2.6.
class DataArchivesFeature : public GameFeature {
public:
    explicit DataArchivesFeature(std::vector<std::string> vanilla_archives)
        : vanilla_archives_(std::move(vanilla_archives)) {}

    static constexpr const char* type_key() { return "data_archives"; }
    const char* type_name() const override { return type_key(); }

    const std::vector<std::string>& vanilla_archives() const {
        return vanilla_archives_;
    }

private:
    std::vector<std::string> vanilla_archives_;
};

// Port of MO2's ScriptExtender: the binary that wraps the game to load
// third-party code (SKSE) and where its plugins live (Data/SKSE/Plugins).
// Loader wiring / detection consume this in P2.3.
class ScriptExtenderFeature : public GameFeature {
public:
    ScriptExtenderFeature(std::string binary_name,
                          std::string plugin_path,
                          std::string loader_name,
                          std::string savegame_extension)
        : binary_name_(std::move(binary_name)),
          plugin_path_(std::move(plugin_path)),
          loader_name_(std::move(loader_name)),
          savegame_extension_(std::move(savegame_extension)) {}

    static constexpr const char* type_key() { return "script_extender"; }
    const char* type_name() const override { return type_key(); }

    const std::string& binary_name() const { return binary_name_; }
    const std::string& plugin_path() const { return plugin_path_; }
    const std::string& loader_name() const { return loader_name_; }
    const std::string& savegame_extension() const { return savegame_extension_; }

private:
    std::string binary_name_;
    std::string plugin_path_;
    std::string loader_name_;
    std::string savegame_extension_;
};

// Port of MO2's ISaveGameInfo: the file extensions a game's saves use
// (Skyrim: ess + the script extender's skse). The Saves tab / missing-assets
// resolution consume this in P2.2.
class SaveGameInfoFeature : public GameFeature {
public:
    explicit SaveGameInfoFeature(std::vector<std::string> savegame_extensions)
        : savegame_extensions_(std::move(savegame_extensions)) {}

    static constexpr const char* type_key() { return "save_game_info"; }
    const char* type_name() const override { return type_key(); }

    const std::vector<std::string>& savegame_extensions() const {
        return savegame_extensions_;
    }

private:
    std::vector<std::string> savegame_extensions_;
};

// Port of MO2's ILocalSavegames: where the game reads/writes saves (subpath
// under the per-game My Games folder) and the INI that records the location.
// Per-profile local saves consume this in P2.2/P2.5.
class LocalSavegamesFeature : public GameFeature {
public:
    LocalSavegamesFeature(std::string saves_subpath, std::string ini_file)
        : saves_subpath_(std::move(saves_subpath)), ini_file_(std::move(ini_file)) {}

    static constexpr const char* type_key() { return "local_savegames"; }
    const char* type_name() const override { return type_key(); }

    const std::string& saves_subpath() const { return saves_subpath_; }
    const std::string& ini_file() const { return ini_file_; }

private:
    std::string saves_subpath_;
    std::string ini_file_;
};

// Port of MO2's IUnmanagedMods: mods the game manages itself (DLC/CC folders,
// stray plugins) that appear in the list as unmanaged rows. ModScanWorker
// merges a registered feature's mods into its unmanaged-row synthesis.
class UnmanagedModsFeature : public GameFeature {
public:
    explicit UnmanagedModsFeature(std::vector<std::string> mods)
        : mods_(std::move(mods)) {}

    static constexpr const char* type_key() { return "unmanaged_mods"; }
    const char* type_name() const override { return type_key(); }

    const std::vector<std::string>& mods() const { return mods_; }

private:
    std::vector<std::string> mods_;
};

// Port of MO2's IBSAInvalidation: the archive a game creates to override
// vanilla BSAs and the version it must be stamped with (0x67 Oblivion,
// 0x68 everything else). BSA invalidation wiring consumes this in P2.6.
class BSAInvalidationFeature : public GameFeature {
public:
    BSAInvalidationFeature(std::string bsa_name, std::string bsa_version)
        : bsa_name_(std::move(bsa_name)), bsa_version_(std::move(bsa_version)) {}

    static constexpr const char* type_key() { return "bsa_invalidation"; }
    const char* type_name() const override { return type_key(); }

    const std::string& bsa_name() const { return bsa_name_; }
    const std::string& bsa_version() const { return bsa_version_; }

private:
    std::string bsa_name_;
    std::string bsa_version_;
};

// Save-game parser feature: a game plugin registers a parser function that
// reads a save file at a given path and returns a populated SaveGame. The
// engine's Saves tab / scan worker resolves this per game_id through the
// GameFeatureRegistry instead of hardcoding game-specific parsers.
class SaveParserFeature : public GameFeature {
public:
    // Parser signature: takes a save file path + game_id, returns a parsed
    // SaveGame. Throws SaveParseError on malformed input.
    using ParserFn = std::function<SaveGame(const std::filesystem::path&,
                                            const std::string& game_id)>;

    explicit SaveParserFeature(ParserFn parser)
        : parser_(std::move(parser)) {}

    static constexpr const char* type_key() { return "save_parser"; }
    const char* type_name() const override { return type_key(); }

    SaveGame parse(const std::filesystem::path& path,
                   const std::string& game_id) const {
        return parser_(path, game_id);
    }

private:
    ParserFn parser_;
};

}  // namespace engine
