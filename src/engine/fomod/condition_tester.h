#pragma once

// Ported from FOMOD Plus (MIT), installer/lib/ConditionTester.h/.cpp. The
// MO2 IOrganizer calls (plugin list state, VFS resolvePath, game version)
// are behind two seams so the engine stays Qt-free:
//   - FomodFileStateResolver  → GameModManager's plugin database / game tree
//   - FomodGameVersionProvider → optional game-version source; when unset,
//     game-version conditions pass with a logged warning.

#include "engine/fomod/module_config.h"
#include "engine/fomod/view_models.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine {

class FomodFileStateResolver {
public:
    virtual ~FomodFileStateResolver() = default;
    // The MO2-equivalent state of a file: Active (present + enabled), Inactive
    // (present + disabled), Missing (not present in the merged view).
    virtual FileDependencyTypeEnum file_state(const std::string& fileName) = 0;
};

class FomodGameVersionProvider {
public:
    virtual ~FomodGameVersionProvider() = default;
    // The game's version string (e.g. "1.6.640"). Compared lexicographically
    // against gameDependency versions, matching MO2 behavior.
    virtual std::string game_version() = 0;
};

class FomodConditionTester {
public:
    FomodConditionTester(FomodFileStateResolver* fileStateResolver,
        FomodGameVersionProvider* gameVersionProvider)
        : mFileStateResolver(fileStateResolver)
        , mGameVersionProvider(gameVersionProvider)
    {
    }

    bool isStepVisible(const std::shared_ptr<FlagMap>& flags, const CompositeDependency& compositeDependency,
        int stepIndex, const std::vector<std::shared_ptr<StepViewModel>>& steps) const;

    bool testCompositeDependency(
        const std::shared_ptr<FlagMap>& flags, const CompositeDependency& compositeDependency) const;

    static bool testFlagDependency(const std::shared_ptr<FlagMap>& flags, const FlagDependency& flagDependency);

    [[nodiscard]] bool testFileDependency(const FileDependency& fileDependency) const;

    bool testGameDependency(const GameDependency& gameDependency) const;

    [[nodiscard]] PluginTypeEnum getPluginTypeDescriptorState(
        const std::shared_ptr<Plugin>& plugin, const std::shared_ptr<FlagMap>& flags) const;

private:
    FomodFileStateResolver* mFileStateResolver;
    FomodGameVersionProvider* mGameVersionProvider;

    friend class FomodViewModel;

    [[nodiscard]] FileDependencyTypeEnum getFileDependencyState(const std::string& fileName) const;

    mutable std::unordered_map<std::string, FileDependencyTypeEnum> fileDependencyCache;
};

}  // namespace engine
