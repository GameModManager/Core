#pragma once

// Ported from FOMOD Plus (MIT), installer/ui/FomodViewModel.h/.cpp. Qt-free:
// the MO2 organizer pointer is replaced by the file-state / game-version
// seams, and preinstall()/getFileInstaller() (MO2 IFileTree plumbing) are
// replaced by the FomodFileInstaller, which FomodStage drives directly.

#include "engine/mod/fomod/condition_tester.h"
#include "engine/mod/fomod/module_config.h"
#include "engine/mod/fomod/view_models.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace engine {

/*
--------------------------------------------------------------------------------
                                Info
--------------------------------------------------------------------------------
*/
class InfoViewModel {
public:
    explicit InfoViewModel(const std::unique_ptr<FomodInfoFile>& infoFile)
    {
        if (infoFile) {
            mName = infoFile->getName();
            mVersion = infoFile->getVersion();
            mAuthor = infoFile->getAuthor();
            mWebsite = infoFile->getWebsite();
        }
    }

    [[nodiscard]] std::string getName() const { return mName; }
    [[nodiscard]] std::string getVersion() const { return mVersion; }
    [[nodiscard]] std::string getAuthor() const { return mAuthor; }
    [[nodiscard]] std::string getWebsite() const { return mWebsite; }

private:
    std::string mName;
    std::string mVersion;
    std::string mAuthor;
    std::string mWebsite;
};

class FomodFileInstaller;

class FomodViewModel {
public:
    FomodViewModel(FomodFileStateResolver* fileStateResolver, FomodGameVersionProvider* gameVersionProvider,
        std::unique_ptr<ModuleConfiguration> fomodFile, std::unique_ptr<FomodInfoFile> infoFile);

    static std::shared_ptr<FomodViewModel> create(FomodFileStateResolver* fileStateResolver,
        FomodGameVersionProvider* gameVersionProvider, std::unique_ptr<ModuleConfiguration> fomodFile,
        std::unique_ptr<FomodInfoFile> infoFile);

    void forEachGroup(const std::function<void(GroupRef)>& callback) const;

    void forEachPlugin(const std::function<void(GroupRef, PluginRef)>& callback) const;

    void forEachFuturePlugin(int fromStepIndex, const std::function<void(GroupRef, PluginRef)>& callback) const;

    void selectFromJson(nlohmann::json json) const;

    void resetToDefaults();

    [[nodiscard]] std::shared_ptr<PluginViewModel> getFirstPluginForActiveStep() const;

    // Steps
    [[nodiscard]] shared_ptr_list<StepViewModel> getSteps() const { return mSteps; }
    [[nodiscard]] StepRef getActiveStep() const { return mActiveStep; }
    [[nodiscard]] int getCurrentStepIndex() const { return mCurrentStepIndex; }

    void updateVisibleSteps() const;

    void rebuildConditionFlagsForStep(int stepIndex) const;

    // The parsed ModuleConfiguration + flags, consumed by FomodFileInstaller.
    [[nodiscard]] const ModuleConfiguration& fomod_file() const { return *mFomodFile; }
    [[nodiscard]] const std::shared_ptr<FlagMap>& flag_map() const { return mFlags; }
    [[nodiscard]] const FomodConditionTester& condition_tester() const { return mConditionTester; }

    [[nodiscard]] std::string getDisplayImage() const;

    // Plugins
    [[nodiscard]] PluginRef getActivePlugin() const { return mActivePlugin; }

    // Info
    [[nodiscard]] std::shared_ptr<InfoViewModel> getInfoViewModel() const { return mInfoViewModel; }

    // Interactions
    void stepBack();

    void stepForward();

    bool isLastVisibleStep() const;

    bool isFirstVisibleStep() const;

    bool togglePlugin(GroupRef, PluginRef, bool selected) const;

    void setActivePlugin(PluginRef plugin) const { mActivePlugin = plugin; }

    static void markManuallySet(PluginRef plugin);

private:
    std::unique_ptr<ModuleConfiguration> mFomodFile;
    std::unique_ptr<FomodInfoFile> mInfoFile;
    std::shared_ptr<FlagMap> mFlags{nullptr};
    FomodConditionTester mConditionTester;
    std::shared_ptr<InfoViewModel> mInfoViewModel;
    std::vector<std::shared_ptr<StepViewModel>> mSteps;
    mutable std::shared_ptr<PluginViewModel> mActivePlugin{nullptr};
    mutable std::shared_ptr<StepViewModel> mActiveStep{nullptr};
    mutable std::vector<int> mVisibleStepIndices;
    bool mInitialized = false;

    void createStepViewModels();

    void setFlagForPluginState(const std::shared_ptr<PluginViewModel>& plugin) const;

    static void createNonePluginForGroup(const std::shared_ptr<GroupViewModel>& group);

    void processPlugin(const std::shared_ptr<GroupViewModel>& group,
        const std::shared_ptr<PluginViewModel>& plugin) const;

    void enforceRadioGroupConstraints(const std::shared_ptr<GroupViewModel>& group) const;

    void enforceSelectAllConstraint(const std::shared_ptr<GroupViewModel>& groupViewModel) const;

    void enforceSelectAtLeastOneConstraint(const std::shared_ptr<GroupViewModel>& group) const;

    void enforceGroupConstraints() const;

    void processPluginConditions(int fromStepIndex) const;

    int mCurrentStepIndex = 0;
};

}  // namespace engine
