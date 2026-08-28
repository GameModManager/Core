#include "engine/mod/fomod/file_installer.h"

#include "engine/mod/fomod/fomod_utils.h"
#include "engine/core/log/logger.h"
#include "engine/core/vfs/path_resolver.h"

#include <algorithm>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>

namespace engine {

FomodFileInstaller::FomodFileInstaller(const std::filesystem::path& modRoot,
    const std::shared_ptr<FomodViewModel>& viewModel)
    : mModRoot(modRoot)
    , mViewModel(viewModel)
{
}

static bool copy_file_to(const std::filesystem::path& src, const std::filesystem::path& dst,
    std::error_code& ec)
{
    std::filesystem::create_directories(dst.parent_path(), ec);
    if (ec)
        return false;
    std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec);
    return !ec;
}

// FOMOD Plus copies a folder's *children* (not the folder itself) into the
// target; an empty destination means "at the tree root". Destinations are
// already validated by the caller (the config-driven path), so the recursion
// just mirrors relative structure - no root escape check needed here.
static void copy_dir_children(const std::filesystem::path& srcDir,
    const std::filesystem::path& dstDir, std::vector<std::string>& missing)
{
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(srcDir, ec)) {
        if (ec)
            break;
        const auto childDst = dstDir / entry.path().filename();
        if (entry.is_directory()) {
            copy_dir_children(entry.path(), childDst, missing);
        } else if (entry.is_regular_file()) {
            if (!copy_file_to(entry.path(), childDst, ec)) {
                missing.push_back(entry.path().string());
            }
        }
    }
}

// Normalize and reject paths that escape the mod root (path traversal).
static std::optional<std::filesystem::path> safe_join(const std::filesystem::path& root,
    const std::string& relative)
{
    const auto rootStr = root.lexically_normal().string();
    const auto joined = (root / relative).lexically_normal();
    if (!joined.string().starts_with(rootStr)) {
        Logger::instance().error("FomodFileInstaller: path escapes mod root, skipping: " + relative);
        return std::nullopt;
    }
    return joined;
}

bool FomodFileInstaller::apply(std::vector<std::string>* missing)
{
    // Build the install into a fresh sibling directory, then swap it in place
    // of the staging dir - mirrors FOMOD Plus building a new orphan tree.
    const auto installTmp = mModRoot.parent_path()
        / (mModRoot.filename().string() + "_gmm_fomod_install");
    std::error_code ec;
    std::filesystem::remove_all(installTmp, ec);
    std::filesystem::create_directories(installTmp, ec);
    if (ec) {
        Logger::instance().error("FomodFileInstaller: failed to create install tree: " + ec.message());
        return false;
    }

    const auto& fomodFile = mViewModel->fomod_file();
    const auto& conditionTester = mViewModel->condition_tester();
    const auto& flags = mViewModel->flag_map();

    const auto filesToInstall = collect_files_to_install(*mViewModel);
    Logger::instance().info("FomodFileInstaller: installing " +
                            std::to_string(filesToInstall.size()) + " files");

    const vfs::PathResolver resolver(mModRoot);
    for (const auto& file : filesToInstall) {
        // FOMOD sources are Windows-native (backslash separators, arbitrary
        // case). PathResolver normalizes separators and matches each component
        // case-insensitively against the on-disk tree.
        const auto gf = resolver.resolve(file.source);
        if (!gf) {
            // Distinguish an escape (absolute / "..") from a plain miss so the
            // log message matches the old resolve_path contract.
            const std::filesystem::path rel(normalize_separators(file.source));
            bool escaped = file.source.empty() || rel.is_absolute();
            for (const auto& part : rel) {
                if (part == "..") { escaped = true; break; }
            }
            if (escaped) {
                Logger::instance().error("FomodFileInstaller: path escapes mod root, skipping: " + file.source);
                continue;
            }
            Logger::instance().error("FomodFileInstaller: could not find source: " + file.source);
            if (missing != nullptr)
                missing->push_back(file.source);
            continue;
        }
        const auto sourcePath = gf->absolute();

        // Destination defaults to the source path; a non-empty <destination>
        // remaps the file (FOMOD Plus uses it verbatim - no auto Data prefix).
        auto targetRel = file.source;
        if (file.destination.has_value() && !file.destination->empty()) {
            targetRel = *file.destination;
        }
        const auto targetPath = safe_join(mModRoot, normalize_separators(targetRel));
        if (!targetPath) {
            continue;
        }

        if (file.isFolder) {
            // Copy the contents of the folder, not the folder itself. An empty
            // destination puts the children at the install root; a non-empty
            // one prefixes them (FOMOD Plus FileInstaller.cpp:58-64).
            std::filesystem::path dstBase = installTmp;
            if (file.destination.has_value() && !file.destination->empty()) {
                const auto safeDest = safe_join(mModRoot, normalize_separators(*file.destination));
                if (!safeDest) {
                    continue;
                }
                dstBase = installTmp / safeDest->lexically_relative(mModRoot);
            }
            copy_dir_children(sourcePath, dstBase, *missing);
        } else {
            // The destination relative path (from the mod root) is what ends up
            // inside the install tree.
            const auto dst = installTmp / targetPath->lexically_relative(mModRoot);
            if (!copy_file_to(sourcePath, dst, ec)) {
                Logger::instance().error("FomodFileInstaller: failed to copy " + file.source);
                if (missing != nullptr)
                    missing->push_back(file.source);
            }
        }
    }

    // Swap the fresh tree in place of the staging dir.
    std::filesystem::remove_all(mModRoot, ec);
    if (ec) {
        Logger::instance().error("FomodFileInstaller: failed to remove staging dir: " + ec.message());
        std::filesystem::remove_all(installTmp, ec);
        return false;
    }
    std::filesystem::rename(installTmp, mModRoot, ec);
    if (ec) {
        Logger::instance().error("FomodFileInstaller: failed to swap install tree: " + ec.message());
        std::filesystem::remove_all(installTmp, ec);
        return false;
    }

    Logger::instance().debug("FomodFileInstaller: install complete");
    return true;
}

std::string FomodFileInstaller::generateFomodJson() const
{
    return generate_fomod_json(*mViewModel);
}

std::vector<engine::File> collect_files_to_install(const FomodViewModel& view_model)
{
    const auto& fomodFile = view_model.fomod_file();
    const auto& conditionTester = view_model.condition_tester();
    const auto& flags = view_model.flag_map();

    std::vector<engine::File> allFiles;

    // Required files from the FOMOD.
    const FileList& requiredInstallFiles = fomodFile.requiredInstallFiles;
    allFiles.insert(allFiles.end(), requiredInstallFiles.files.begin(), requiredInstallFiles.files.end());

    // Selected files from visible steps.
    for (const auto& stepViewModel : view_model.getSteps()) {
        if (!conditionTester.testCompositeDependency(flags, stepViewModel->getVisibilityConditions())) {
            continue;
        }
        for (const auto& groupViewModel : stepViewModel->getGroups()) {
            for (const auto& pluginViewModel : groupViewModel->getPlugins()) {
                if (pluginViewModel->isSelected()) {
                    const auto& files = pluginViewModel->getPlugin()->files.files;
                    allFiles.insert(allFiles.end(), files.begin(), files.end());
                }
            }
        }
    }

    // Conditional install files.
    for (const auto& pattern : fomodFile.conditionalFileInstalls.patterns) {
        if (conditionTester.testCompositeDependency(flags, pattern.dependencies)) {
            const auto& files = pattern.files.files;
            allFiles.insert(allFiles.end(), files.begin(), files.end());
        }
    }

    // Stable sort by ascending priority: equal-priority files keep their XML
    // order, so a later entry overwrites an earlier one at the same priority.
    std::stable_sort(allFiles.begin(), allFiles.end(),
        [](const engine::File& a, const engine::File& b) { return a.priority < b.priority; });

    return allFiles;
}

std::string generate_fomod_json(const FomodViewModel& view_model)
{
    nlohmann::json fomodJson;
    fomodJson["steps"] = nlohmann::json::array();
    for (const auto& stepViewModel : view_model.getSteps()) {
        auto stepJson = nlohmann::json::object();
        stepJson["name"] = stepViewModel->getName();
        stepJson["groups"] = nlohmann::json::array();

        for (const auto& groupViewModel : stepViewModel->getGroups()) {
            auto groupJson = nlohmann::json::object();
            groupJson["name"] = groupViewModel->getName();
            auto pluginArray = nlohmann::json::array();
            auto deselectedArray = nlohmann::json::array();

            for (const auto& pluginViewModel : groupViewModel->getPlugins()) {
                if (pluginViewModel->isSelected()) {
                    pluginArray.emplace_back(pluginViewModel->getName());
                }
                if (!pluginViewModel->isSelected() && pluginViewModel->wasManuallySet()) {
                    deselectedArray.emplace_back(pluginViewModel->getName());
                }
            }
            groupJson["plugins"] = pluginArray;
            groupJson["deselected"] = deselectedArray;
            stepJson["groups"].emplace_back(groupJson);
        }
        fomodJson["steps"].emplace_back(stepJson);
    }
    return fomodJson.dump();
}

}  // namespace engine
