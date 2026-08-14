#include "engine/mod/fomod/fomod_view_model.h"

#include "engine/mod/fomod/fomod_utils.h"
#include "engine/core/log/logger.h"

#include <algorithm>
#include <iterator>
#include <ranges>
#include <sstream>
#include <utility>

namespace engine {

using GroupCallback = std::function<void(GroupRef)>;
using PluginCallback = std::function<void(GroupRef, PluginRef)>;

/*
--------------------------------------------------------------------------------
                               Helpers
--------------------------------------------------------------------------------
*/

static bool isRadioLike(GroupRef group)
{
    return group->getType() == SelectExactlyOne
        || (group->getType() == SelectAtMostOne && group->getPlugins().size() > 1);
}

static bool moreThanOneSelected(GroupRef group)
{
    int selected = 0;
    for (const auto& plugin : group->getPlugins()) {
        if (plugin->isSelected())
            ++selected;
    }
    return selected > 1;
}

static bool anySelected(GroupRef group)
{
    return std::any_of(group->getPlugins().begin(), group->getPlugins().end(),
        [](const auto& plugin) { return plugin->isSelected(); });
}

static std::string pluginTypeEnumToString(const PluginTypeEnum type)
{
    switch (type) {
    case PluginTypeEnum::Recommended:
        return "Recommended";
    case PluginTypeEnum::Required:
        return "Required";
    case PluginTypeEnum::Optional:
        return "Optional";
    case PluginTypeEnum::NotUsable:
        return "NotUsable";
    case PluginTypeEnum::CouldBeUsable:
        return "CouldBeUsable";
    default:
        return "Unknown";
    }
}

/*
--------------------------------------------------------------------------------
                               ViewModel Lifecycle
--------------------------------------------------------------------------------
*/

FomodViewModel::FomodViewModel(FomodFileStateResolver* fileStateResolver,
    FomodGameVersionProvider* gameVersionProvider, std::unique_ptr<ModuleConfiguration> fomodFile,
    std::unique_ptr<FomodInfoFile> infoFile)
    : mFomodFile(std::move(fomodFile))
    , mInfoFile(std::move(infoFile))
    , mConditionTester(fileStateResolver, gameVersionProvider)
    , mInfoViewModel(std::make_shared<InfoViewModel>(mInfoFile))
{
    mFlags = std::make_shared<FlagMap>();
}

std::shared_ptr<FomodViewModel> FomodViewModel::create(FomodFileStateResolver* fileStateResolver,
    FomodGameVersionProvider* gameVersionProvider, std::unique_ptr<ModuleConfiguration> fomodFile,
    std::unique_ptr<FomodInfoFile> infoFile)
{
    auto viewModel = std::make_shared<FomodViewModel>(
        fileStateResolver, gameVersionProvider, std::move(fomodFile), std::move(infoFile));
    if (viewModel->mFlags == nullptr) {
        viewModel->mFlags = std::make_shared<FlagMap>();
    }
    viewModel->createStepViewModels();

    // Handle FOMODs with no steps
    if (viewModel->mSteps.empty()) {
        viewModel->mInitialized = true;
        Logger::instance().info("FomodViewModel: FOMOD with no steps - initialization complete");
        return viewModel;
    }

    viewModel->processPluginConditions(-1);
    viewModel->enforceGroupConstraints();
    viewModel->updateVisibleSteps();
    viewModel->mInitialized = true;
    viewModel->mCurrentStepIndex = viewModel->mVisibleStepIndices.front();
    viewModel->mActiveStep = viewModel->mSteps.at(viewModel->mVisibleStepIndices.front());
    viewModel->mActivePlugin = viewModel->getFirstPluginForActiveStep();
    viewModel->getActiveStep()->setVisited(true);
    return viewModel;
}

/*
--------------------------------------------------------------------------------
                               Traversal Functions
--------------------------------------------------------------------------------
*/

void FomodViewModel::forEachGroup(const GroupCallback& callback) const
{
    for (const auto& stepViewModel : mSteps) {
        for (const auto& groupViewModel : stepViewModel->getGroups()) {
            callback(groupViewModel);
        }
    }
}

void FomodViewModel::forEachPlugin(const PluginCallback& callback) const
{
    for (const auto& stepViewModel : mSteps) {
        for (const auto& groupViewModel : stepViewModel->getGroups()) {
            for (const auto& pluginViewModel : groupViewModel->getPlugins()) {
                callback(groupViewModel, pluginViewModel);
            }
        }
    }
}

void FomodViewModel::forEachFuturePlugin(const int fromStepIndex, const PluginCallback& callback) const
{
    for (int i = fromStepIndex + 1; i < static_cast<int>(mSteps.size()); ++i) {
        for (const auto& groupViewModel : mSteps[i]->getGroups()) {
            for (const auto& pluginViewModel : groupViewModel->getPlugins()) {
                callback(groupViewModel, pluginViewModel);
            }
        }
    }
}

/*
--------------------------------------------------------------------------------
                               Initializers
--------------------------------------------------------------------------------
*/
void FomodViewModel::createStepViewModels()
{
    // Handle legacy FOMODs with no install steps
    if (mFomodFile->installSteps.installSteps.empty()) {
        Logger::instance().info("FomodViewModel: No install steps found - creating default step for legacy FOMOD");
        return;
    }

    shared_ptr_list<StepViewModel> stepViewModels;

    for (int stepIndex = 0; stepIndex < static_cast<int>(mFomodFile->installSteps.installSteps.size()); ++stepIndex) {
        const auto& installStep = mFomodFile->installSteps.installSteps[stepIndex];
        shared_ptr_list<GroupViewModel> groupViewModels;

        for (int groupIndex = 0; groupIndex < static_cast<int>(installStep.optionalFileGroups.groups.size());
             ++groupIndex) {
            const auto& group = installStep.optionalFileGroups.groups[groupIndex];
            shared_ptr_list<PluginViewModel> pluginViewModels;

            for (int pluginIndex = 0; pluginIndex < static_cast<int>(group.plugins.plugins.size()); ++pluginIndex) {
                const auto& plugin = group.plugins.plugins[pluginIndex];
                auto pluginViewModel
                    = std::make_shared<PluginViewModel>(std::make_shared<Plugin>(plugin), false, true, pluginIndex);

                pluginViewModel->setStepIndex(stepIndex);
                pluginViewModel->setGroupIndex(groupIndex);
                pluginViewModels.emplace_back(pluginViewModel);
            }
            auto groupViewModel = std::make_shared<GroupViewModel>(
                std::make_shared<Group>(group), pluginViewModels, groupIndex, stepIndex);
            if (groupViewModel->getType() == SelectAtMostOne && groupViewModel->getPlugins().size() > 1) {
                createNonePluginForGroup(groupViewModel);
            }
            groupViewModels.emplace_back(groupViewModel);
        }
        auto stepViewModel = std::make_shared<StepViewModel>(
            std::make_shared<InstallStep>(installStep), std::move(groupViewModels), stepIndex);
        stepViewModels.emplace_back(stepViewModel);
    }
    mSteps = std::move(stepViewModels);
}

void FomodViewModel::createNonePluginForGroup(GroupRef group)
{
    const auto nonePlugin = std::make_shared<Plugin>();
    nonePlugin->name = "None";
    nonePlugin->typeDescriptor.type = PluginTypeEnum::Optional;
    const int newIndex = static_cast<int>(group->getPlugins().size());
    const auto nonePluginViewModel = std::make_shared<PluginViewModel>(nonePlugin, true, true, newIndex);
    group->addPlugin(nonePluginViewModel);
}

/*
--------------------------------------------------------------------------------
                               Group Constraints
--------------------------------------------------------------------------------
*/

void FomodViewModel::enforceRadioGroupConstraints(GroupRef group) const
{
    if (!isRadioLike(group)) {
        return;
    }

    if (group->getType() == SelectExactlyOne && group->getPlugins().size() == 1) {
        group->getPlugins().at(0)->setEnabled(false);
    }

    if (moreThanOneSelected(group)) {
        for (const auto& plugin : group->getPlugins()) {
            plugin->setSelected(false);  // don't call toggle here, that'll do the radio stuff.
        }
    }

    if (anySelected(group)) {
        return;
    }

    // First, try to select the first Recommended plugin
    for (const auto& plugin : group->getPlugins()) {
        if (mConditionTester.getPluginTypeDescriptorState(plugin->getPlugin(), mFlags) == PluginTypeEnum::Recommended) {
            togglePlugin(group, plugin, true);
            return;
        }
    }

    // If no Recommended plugin is found, select the first one that isn't NotUsable
    for (const auto& plugin : group->getPlugins()) {
        if (mConditionTester.getPluginTypeDescriptorState(plugin->getPlugin(), mFlags) != PluginTypeEnum::NotUsable) {
            togglePlugin(group, plugin, true);
            return;
        }
    }
}

void FomodViewModel::enforceSelectAllConstraint(GroupRef groupViewModel) const
{
    if (groupViewModel->getType() != SelectAll) {
        return;
    }

    for (const auto& pluginViewModel : groupViewModel->getPlugins()) {
        togglePlugin(groupViewModel, pluginViewModel, true);
        pluginViewModel->setEnabled(false);
    }
}

void FomodViewModel::enforceSelectAtLeastOneConstraint(GroupRef group) const
{
    if (group->getType() != SelectAtLeastOne || group->getPlugins().size() != 1) {
        return;
    }

    const auto plugin = group->getPlugins().front();
    if (mConditionTester.getPluginTypeDescriptorState(plugin->getPlugin(), mFlags) != PluginTypeEnum::NotUsable) {
        togglePlugin(group, plugin, true);
        plugin->setEnabled(false);
    }
}

void FomodViewModel::enforceGroupConstraints() const
{
    forEachGroup([this](const auto& groupViewModel) {
        enforceRadioGroupConstraints(groupViewModel);
        enforceSelectAllConstraint(groupViewModel);
        enforceSelectAtLeastOneConstraint(groupViewModel);
    });
}

/*
--------------------------------------------------------------------------------
                               Plugin Constraints
--------------------------------------------------------------------------------
*/

void FomodViewModel::processPlugin(GroupRef group, PluginRef plugin) const
{
    if (group->getType() == SelectAll) {
        return;
    }
    const auto typeDescriptor = mConditionTester.getPluginTypeDescriptorState(plugin->getPlugin(), mFlags);

    if (typeDescriptor == plugin->getCurrentPluginType()) {
        return;
    }

    plugin->setCurrentPluginType(typeDescriptor);

    const bool isOnlyPlugin = group->getPlugins().size() == 1
        && (group->getType() == SelectExactlyOne || group->getType() == SelectAtLeastOne);

    // check if step hasVisited, if it hasn't been, set it to unchecked if it's optional.
    const auto stepNotVisitedYet = !mSteps[group->getStepIndex()]->getHasVisited();

    switch (typeDescriptor) {
    case PluginTypeEnum::Recommended:
        plugin->setEnabled(true);
        if (!plugin->isSelected()) {
            togglePlugin(group, plugin, true);
        }
        break;
    case PluginTypeEnum::Required:
        plugin->setEnabled(false);
        if (!plugin->isSelected()) {
            togglePlugin(group, plugin, true);
        }
        break;
    case PluginTypeEnum::Optional:
        if (!isOnlyPlugin) {
            plugin->setEnabled(true);
        }
        // In the case where we're changing flags to make something optional from Recommended, set it back to unchecked.
        if (plugin->isSelected() && stepNotVisitedYet && group->getType() == SelectAny) {
            togglePlugin(group, plugin, false);
        }
        break;
    case PluginTypeEnum::NotUsable:
        plugin->setEnabled(false);
        if (plugin->isSelected()) {
            togglePlugin(group, plugin, false);
        }
        break;
    case PluginTypeEnum::CouldBeUsable:
        plugin->setEnabled(true);
        break;
    default:;
    }
}

void FomodViewModel::processPluginConditions(const int fromStepIndex) const
{
    // We only want to update plugins that haven't been seen yet. Otherwise, we could undo manual selections by the
    // user.
    if (fromStepIndex >= 0) {
        forEachFuturePlugin(fromStepIndex, [this](const auto& groupViewModel, const auto& pluginViewModel) {
            processPlugin(groupViewModel, pluginViewModel);
        });
    } else {
        forEachPlugin([this](const auto& groupViewModel, const auto& pluginViewModel) {
            processPlugin(groupViewModel, pluginViewModel);
        });
    }
}

void FomodViewModel::setFlagForPluginState(PluginRef plugin) const
{
    if (plugin->isSelected()) {
        mFlags->setFlagsForPlugin(plugin);
    } else {
        mFlags->unsetFlagsForPlugin(plugin);
    }
}

bool FomodViewModel::togglePlugin(GroupRef group, PluginRef plugin, const bool selected) const
{
    if (plugin->isSelected() == selected) {
        return false;
    }

    // Disable other radio options first.
    if (selected && isRadioLike(group)) {
        for (const auto& otherPlugin : group->getPlugins()) {
            if (otherPlugin != plugin && otherPlugin->isSelected()) {
                otherPlugin->setSelected(false);
                setFlagForPluginState(otherPlugin);
            }
        }
    }

    const auto stepIndex = group->getStepIndex();

    plugin->setSelected(selected);
    setFlagForPluginState(plugin);

    if (mInitialized) {
        mActivePlugin = plugin;
    }
    processPluginConditions(stepIndex);
    updateVisibleSteps();
    return true;
}

void FomodViewModel::markManuallySet(PluginRef plugin) { plugin->manuallySet = true; }

/*
--------------------------------------------------------------------------------
                               Step Constraints
--------------------------------------------------------------------------------
*/

void FomodViewModel::updateVisibleSteps() const
{
    mVisibleStepIndices.clear();
    mFlags->clearAll();

    for (int i = 0; i < static_cast<int>(mSteps.size()); ++i) {
        if (i == 0) {
            rebuildConditionFlagsForStep(i);
        }

        // This also depends on previous flags that may have set this particular flag.
        if (mConditionTester.isStepVisible(mFlags, mSteps[i]->getVisibilityConditions(), i, mSteps)) {
            mVisibleStepIndices.push_back(i);
            rebuildConditionFlagsForStep(i);
        }
    }
}

void FomodViewModel::rebuildConditionFlagsForStep(const int stepIndex) const
{
    for (const auto& group : mSteps[stepIndex]->getGroups()) {
        for (const auto& plugin : group->getPlugins()) {
            setFlagForPluginState(plugin);
        }
    }
}

/*
--------------------------------------------------------------------------------
                               Navigation/UI
--------------------------------------------------------------------------------
*/

void FomodViewModel::stepBack()
{
    if (mSteps.empty()) {
        return;  // No steps to move back to
    }

    const auto it = std::find(mVisibleStepIndices.begin(), mVisibleStepIndices.end(), mCurrentStepIndex);
    if (it != mVisibleStepIndices.end() && it != mVisibleStepIndices.begin()) {
        mCurrentStepIndex = *std::prev(it);
        mActiveStep = mSteps[mCurrentStepIndex];
        mActivePlugin = getFirstPluginForActiveStep();
    }
}

void FomodViewModel::stepForward()
{
    if (mSteps.empty()) {
        return;  // No steps to move forward to
    }

    const auto it = std::find(mVisibleStepIndices.begin(), mVisibleStepIndices.end(), mCurrentStepIndex);
    if (it != mVisibleStepIndices.end() && std::next(it) != mVisibleStepIndices.end()) {
        mCurrentStepIndex = *std::next(it);
        mActiveStep = mSteps[mCurrentStepIndex];
        mActivePlugin = getFirstPluginForActiveStep();
    }
    mActiveStep->setVisited(true);
}

bool FomodViewModel::isLastVisibleStep() const
{
    if (mSteps.empty()) {
        return true;  // Legacy FOMODs are always "last step"
    }
    return !mVisibleStepIndices.empty() && mCurrentStepIndex == mVisibleStepIndices.back();
}

bool FomodViewModel::isFirstVisibleStep() const
{
    if (mSteps.empty()) {
        return true;  // Legacy FOMODs are always "first step"
    }
    return !mVisibleStepIndices.empty() && mCurrentStepIndex == mVisibleStepIndices.front();
}

std::string FomodViewModel::getDisplayImage() const
{
    // if the active plugin has an image, return it
    if (mActivePlugin && !mActivePlugin->getImagePath().empty()) {
        return mActivePlugin->getImagePath();
    }
    return mCurrentStepIndex == 0 ? mFomodFile->moduleImage.path : "";
}

std::shared_ptr<PluginViewModel> FomodViewModel::getFirstPluginForActiveStep() const
{
    if (!mActiveStep) {
        return nullptr;
    }

    const auto& groups = mActiveStep->getGroups();
    if (groups.empty()) {
        return nullptr;
    }

    const auto& plugins = groups.front()->getPlugins();
    if (plugins.empty()) {
        return nullptr;
    }

    return plugins.front();
}

/*
--------------------------------------------------------------------------------
                               Utility
--------------------------------------------------------------------------------
*/
void FomodViewModel::resetToDefaults()
{
    // Clear all flags first
    mFlags->clearAll();

    // Reset all plugins to deselected and clear visited states
    for (const auto& step : mSteps) {
        step->setVisited(false);
        for (const auto& group : step->getGroups()) {
            for (const auto& plugin : group->getPlugins()) {
                plugin->setSelected(false);
                plugin->setEnabled(true);
                plugin->manuallySet = false;
                plugin->setCurrentPluginType(PluginTypeEnum::UNKNOWN);
            }
        }
    }

    // Re-run the initial constraint enforcement to restore author defaults
    processPluginConditions(-1);
    enforceGroupConstraints();
    updateVisibleSteps();

    // Reset to first step
    mCurrentStepIndex = mVisibleStepIndices.empty() ? 0 : mVisibleStepIndices.front();
    mActiveStep = mSteps.empty() ? nullptr : mSteps.at(mCurrentStepIndex);
    mActivePlugin = getFirstPluginForActiveStep();
    if (mActiveStep) {
        mActiveStep->setVisited(true);
    }
}

void FomodViewModel::selectFromJson(nlohmann::json json) const
{
    if (!json.contains("steps") || !json["steps"].is_array()) {
        return;
    }

    const auto jsonSteps = json["steps"];
    const auto stepCount = jsonSteps.size();

    for (int stepIndex = 0; stepIndex < static_cast<int>(stepCount); ++stepIndex) {
        if (stepIndex > static_cast<int>(mSteps.size()) - 1) {
            continue;
        }

        const auto currentStep = mSteps[stepIndex];
        const auto step = jsonSteps[stepIndex];
        if (!step.contains("groups") || !step["groups"].is_array()) {
            continue;
        }
        const auto groupCount = step["groups"].size();

        for (int groupIndex = 0; groupIndex < static_cast<int>(groupCount); ++groupIndex) {
            if (groupIndex > static_cast<int>(currentStep->getGroups().size()) - 1) {
                continue;
            }

            const auto group = step["groups"][groupIndex];
            const auto currentGroup = currentStep->getGroups()[groupIndex];

            if (group.contains("plugins") && group["plugins"].is_array()) {
                for (const auto& jsonPlugin : group["plugins"]) {
                    const auto& allPlugins = currentGroup->getPlugins();
                    const auto searchName = jsonPlugin.get<std::string>();

                    const auto currentPlugin = std::find_if(
                        allPlugins.begin(), allPlugins.end(),
                        [searchName](PluginRef p) { return p->getName() == searchName; });

                    if (currentPlugin == allPlugins.end()) {
                        continue;
                    }

                    if ((*currentPlugin)->isSelected()) {
                        continue;
                    }
                    if (!(*currentPlugin)->isEnabled()) {
                        continue;
                    }
                    togglePlugin(currentGroup, *currentPlugin, true);
                }
            }

            if (!group.contains("deselected")) {
                continue;
            }

            // Do the opposite of the above. For unchecked plugins, disable them.
            for (const auto& jsonPlugin : group["deselected"]) {
                const auto& allPlugins = currentGroup->getPlugins();
                const auto searchName = jsonPlugin.get<std::string>();

                const auto currentPlugin = std::find_if(
                    allPlugins.begin(), allPlugins.end(),
                    [searchName](PluginRef p) { return p->getName() == searchName; });

                if (currentPlugin == allPlugins.end()) {
                    continue;
                }

                if (!(*currentPlugin)->isSelected()) {
                    continue;
                }
                if (!(*currentPlugin)->isEnabled()) {
                    continue;
                }
                togglePlugin(currentGroup, *currentPlugin, false);
                (*currentPlugin)->manuallySet = true;  // To preserve this state when serializing JSON.
            }
        }
    }
}

}  // namespace engine
