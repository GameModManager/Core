#include "engine/pipeline/fomod_stage.h"
#include "engine/pipeline/pipeline.h"
#include "engine/model/mod.h"
#include "engine/fomod/condition_tester.h"
#include "engine/fomod/file_installer.h"
#include "engine/fomod/fomod_utils.h"
#include "engine/fomod/fomod_view_model.h"
#include "engine/fomod/module_config.h"
#include "engine/meta/mod_meta.h"
#include "engine/log/logger.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace engine {

namespace {

// File-state resolution for FileDependencyType conditions at install time
// (FOMOD Plus resolves against the in-progress mod tree + game data): a file
// is Active when present in the archive content being installed or in the
// game's data directory; Inactive is not distinguishable here, so everything
// present resolves to Active.
class StagingFileStateResolver final : public FomodFileStateResolver {
public:
    StagingFileStateResolver(std::filesystem::path contentRoot, std::filesystem::path gameDataDir)
        : contentRoot_(std::move(contentRoot))
        , gameDataDir_(std::move(gameDataDir))
    {
    }

    FileDependencyTypeEnum file_state(const std::string& fileName) override
    {
        // FOMOD paths are Windows-native; resolve case-insensitively so a
        // condition referencing "meshes\Skeleton.nif" matches Meshes/...
        if (!resolve_path(contentRoot_, fileName).empty()) {
            return FileDependencyTypeEnum::Active;
        }
        if (!gameDataDir_.empty() && !resolve_path(gameDataDir_, fileName).empty()) {
            return FileDependencyTypeEnum::Active;
        }
        return FileDependencyTypeEnum::Missing;
    }

private:
    std::filesystem::path contentRoot_;
    std::filesystem::path gameDataDir_;
};

// FOMOD Plus findFomodDirectory: a directory named "fomod" (any casing) wins;
// else if the current directory has exactly one entry and it is a directory,
// descend into it.
std::optional<std::filesystem::path> find_fomod_dir(const std::filesystem::path& dir)
{
    std::error_code ec;
    std::optional<std::filesystem::path> singleChild;
    int entryCount = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) {
            return std::nullopt;
        }
        if (entry.is_directory() && name_matches_ci(entry.path(), std::string(fomod_files::FOMOD_DIR))) {
            return entry.path();
        }
        singleChild = entry.path();
        ++entryCount;
    }
    if (entryCount == 1 && singleChild && std::filesystem::is_directory(*singleChild, ec)) {
        return find_fomod_dir(*singleChild);
    }
    return std::nullopt;
}

// Previously persisted FOMOD choices from a reinstall: read [fomod] choices
// from the mod folder's meta.ini (written by InstallStage).
std::string read_previous_choices(const std::filesystem::path& modsDir, const std::string& folderName)
{
    if (modsDir.empty() || folderName.empty()) {
        return {};
    }
    const auto metaPath = modsDir / folderName / "meta.ini";
    std::ifstream f(metaPath);
    if (!f) {
        return {};
    }
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    ModMeta meta;
    if (!meta.parse(content)) {
        return {};
    }
    return meta.get("fomod", "choices");
}

// Flatten content_root/* into staging_root (used when find_fomod_dir descended
// into a single wrapper directory), so InstallStage copies a flat tree.
bool flatten_into(const std::filesystem::path& stagingRoot, const std::filesystem::path& contentRoot)
{
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(contentRoot, ec)) {
        if (ec) {
            return false;
        }
        const auto dest = stagingRoot / entry.path().filename();
        std::filesystem::rename(entry.path(), dest, ec);
        if (ec) {
            return false;
        }
    }
    std::filesystem::remove_all(contentRoot, ec);
    return !ec;
}

}  // namespace

bool FomodStage::execute(Mod& mod, PipelineContext& ctx)
{
    // Locate the extracted mod root from the staging dir entry pushed by
    // ExtractStage (same heuristic as InstallStage).
    std::filesystem::path stagingRoot;
    for (const auto& f : mod.files) {
        auto p = std::filesystem::path(f.relative_path);
        if (std::filesystem::is_directory(p)) {
            stagingRoot = p;
            break;
        }
    }

    if (stagingRoot.empty()) {
        Logger::instance().debug("FomodStage: no extracted directory found, nothing to check");
        return true;
    }

    // Not a FOMOD unless a fomod/ dir with ModuleConfig.xml is present
    // (FOMOD Plus isArchiveSupported).
    const auto fomodDir = find_fomod_dir(stagingRoot);
    if (!fomodDir) {
        return true;
    }
    const auto moduleConfigPath = find_file_ci(*fomodDir, toLower(std::string(fomod_files::MODULE_CONFIG)));
    if (moduleConfigPath.empty()) {
        return true;
    }

    Logger::instance().info("FomodStage: FOMOD installer detected for '" + mod.name + "'");

    // Content root = the directory that contains fomod/ (FOMOD Plus's
    // mFomodPath). FOMOD file sources are relative to it.
    const auto contentRoot = fomodDir->parent_path();

    auto moduleConfig = std::make_unique<ModuleConfiguration>();
    try {
        moduleConfig->deserialize(moduleConfigPath);
    } catch (const XmlParseException& e) {
        Logger::instance().error("FomodStage: error parsing ModuleConfig.xml: " + std::string(e.what()));
        return false;
    }

    // C#-script installers are not supported (the FOMOD spec lets authors ship
    // arbitrary C#). Abort with a clear message instead of a wrong install.
    if (moduleConfig->hasCSharpScript()) {
        Logger::instance().warn("FomodStage: '<csharpScript>' FOMOD installers are not supported - "
                                "install '" + mod.name + "' manually");
        return false;
    }

    auto infoFile = std::make_unique<FomodInfoFile>();
    const auto infoPath = find_file_ci(*fomodDir, std::string(fomod_files::INFO_XML));
    if (!infoPath.empty()) {
        try {
            infoFile->deserialize(infoPath);
        } catch (const XmlParseException& e) {
            // info.xml is optional; a broken one only costs the metadata.
            Logger::instance().error("FomodStage: error parsing info.xml: " + std::string(e.what()));
        }
    }

    // Version from info.xml when the download didn't provide one (FOMOD Plus).
    if (mod.version.empty() && !infoFile->getVersion().empty()) {
        mod.version = infoFile->getVersion();
    }

    // Suggested name: info.xml name wins over the archive-derived one
    // (FOMOD Plus GUESS_FALLBACK). The wizard may override it via the decision.
    std::string suggestedName = infoFile->getName().empty() ? mod.name : infoFile->getName();
    if (suggestedName.empty()) {
        suggestedName = mod.id;
    }

    // Previous choices from a reinstall, matched by mod folder name.
    const std::string previousChoices = read_previous_choices(ctx.mods_dir, suggestedName);

    // View model with the staging resolver; no game-version source exists yet,
    // so version conditions pass with a logged warning (the documented default
    // keeps author-recommended selections visible).
    std::filesystem::path gameDataDir = ctx.game_dir;
    if (!ctx.game_dir.empty() && std::filesystem::is_directory(ctx.game_dir / ctx.deploy_prefix)) {
        gameDataDir = ctx.game_dir / ctx.deploy_prefix;
    }
    StagingFileStateResolver resolver(contentRoot, gameDataDir);
    auto viewModel = FomodViewModel::create(&resolver, nullptr, std::move(moduleConfig), std::move(infoFile));

    std::string choicesJson;
    std::string modName;
    bool ignoreMissing = false;

    if (ctx.fomod_query_cb) {
        const auto decision = ctx.fomod_query_cb(viewModel, contentRoot, suggestedName, previousChoices);
        if (decision.manual) {
            // Manual install: skip the wizard's option selection and install
            // the archive contents as-is. fomod/ is installer metadata and
            // never becomes part of the mod (same rule as the wizard path).
            std::error_code ec;
            std::filesystem::remove_all(*fomodDir, ec);
            if (contentRoot != stagingRoot) {
                if (!flatten_into(stagingRoot, contentRoot)) {
                    Logger::instance().error("FomodStage: failed to flatten manual install");
                    return false;
                }
            }
            mod.name = decision.mod_name.empty() ? suggestedName : decision.mod_name;
            if (mod.name.empty()) {
                mod.name = mod.id;
            }
            Logger::instance().debug("FomodStage: manual install (FOMOD wizard skipped)");
            return true;
        }
        if (!decision.accept) {
            Logger::instance().debug("FomodStage: FOMOD install canceled by user");
            return false;
        }
        choicesJson = decision.choices_json;
        modName = decision.mod_name;
        ignoreMissing = decision.ignore_missing;
    } else {
        // Headless: restore previously persisted choices; without them there is
        // no way to know what the user wants - abort rather than guess.
        if (previousChoices.empty()) {
            Logger::instance().error("FomodStage: FOMOD installer requires a wizard - run the install "
                                     "from the GUI (mod '" + mod.name + "')");
            return false;
        }
        choicesJson = previousChoices;
    }

    // Apply restored/returned choices (defaults remain when none are given).
    if (!choicesJson.empty()) {
        try {
            viewModel->selectFromJson(nlohmann::json::parse(choicesJson));
        } catch (...) {
            Logger::instance().warn("FomodStage: failed to parse previous choices; using defaults");
        }
    }

    FomodFileInstaller installer(contentRoot, viewModel);
    std::vector<std::string> missing;
    if (!installer.apply(&missing)) {
        return false;
    }

    if (!missing.empty()) {
        for (const auto& m : missing) {
            Logger::instance().warn("FomodStage: FOMOD references a file missing from the archive: " + m);
        }
        if (!ignoreMissing) {
            Logger::instance().error("FomodStage: files referenced by the FOMOD were missing from the "
                                     "archive - install aborted; re-run and accept the missing-file "
                                     "warning to force the install");
            return false;
        }
    }

    // If the fomod dir was nested inside a single wrapper directory, flatten
    // the installed content up to the staging root for InstallStage.
    if (contentRoot != stagingRoot) {
        if (!flatten_into(stagingRoot, contentRoot)) {
            Logger::instance().error("FomodStage: failed to flatten installed content");
            return false;
        }
    }

    ctx.fomod_choices_json = choicesJson;
    if (!modName.empty()) {
        mod.name = modName;
    } else if (!suggestedName.empty()) {
        mod.name = suggestedName;
    }

    Logger::instance().debug("FomodStage: FOMOD install applied");
    return true;
}

}  // namespace engine
