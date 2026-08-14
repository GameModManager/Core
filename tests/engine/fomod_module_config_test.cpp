// FOMOD ModuleConfig.xml parser — validates against the same real-world mod
// fixtures FOMOD Plus's own tests use (tests/moduleconf/ in the upstream repo,
// MIT; the XML fixtures are vendored into fomod_fixtures/). The assertions
// mirror the upstream test files so a parser regression here means the same
// thing it would upstream.

#include "engine/mod/fomod/module_config.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <catch2/catch_test_macros.hpp>

namespace {

using namespace engine;

struct Fixture {
    const char* file;
    const char* module_name;
    int step_count;
};

const Fixture kFixtures[] = {
    {"test_moduleconf_bos.xml", "Base Object Swapper", -1},
    {"test_moduleconf_citytrees.xml", "RogueUnicorn - City Trees", 8},
    {"test_moduleconf_embers.xml", "Embers XD", 5},
    {"test_moduleconf_lux.xml", "Lux (patch hub)", 5},
    {"test_moduleconf_miniembers.xml", "Embers XD", -1},
    {"test_moduleconf_precision.xml", "Precision", -1},
    {"test_moduleconf_xavbio.xml", "xavbio's meshes for 3BA", 3},
};

std::filesystem::path fixture_path(const std::string& name)
{
    return std::filesystem::path(__FILE__).parent_path() / "fomod_fixtures" / name;
}

ModuleConfiguration parse(const std::string& name)
{
    ModuleConfiguration config;
    REQUIRE(config.deserialize(fixture_path(name)));
    return config;
}

}  // namespace

TEST_CASE("fomod module config", "[engine]") {
    // All real-world fixtures parse with the expected module name and step
    // count, and every step carries at least one group with plugins.
    for (const auto& fix : kFixtures) {
        auto config = parse(fix.file);
        REQUIRE(config.moduleName == fix.module_name);
        if (fix.step_count >= 0) {
            REQUIRE(static_cast<int>(config.installSteps.installSteps.size()) == fix.step_count);
        }
        REQUIRE(!config.installSteps.installSteps.empty());
        for (const auto& step : config.installSteps.installSteps) {
            REQUIRE(!step.name.empty());
            REQUIRE(!step.optionalFileGroups.groups.empty());
            for (const auto& group : step.optionalFileGroups.groups) {
                REQUIRE(!group.plugins.plugins.empty());
            }
        }
        std::printf("PASS: fomod_module_config — %s parses\n", fix.file);
    }

    // Precision (test_moduleconf_precision.cpp): module name, required files,
    // and the TK Dodge typeDescriptor (file dependencies, order, priority).
    {
        auto config = parse("test_moduleconf_precision.xml");
        REQUIRE(config.moduleName == "Precision");

        const auto& required = config.requiredInstallFiles.files;
        REQUIRE(required.size() == 8);
        REQUIRE(required[0].source == "Interface");
        REQUIRE(required[0].destination == "Interface");
        REQUIRE(required[7].source == "Precision.esp");
        REQUIRE(required[7].destination == "Precision.esp");

        const auto& tkDodge = config.installSteps.installSteps[0]
                                  .optionalFileGroups.groups[0].plugins.plugins[0];
        REQUIRE(tkDodge.typeDescriptor.dependencyType.defaultType == PluginTypeEnum::NotUsable);

        const auto& pattern = tkDodge.typeDescriptor.dependencyType.patterns.patterns[0];
        REQUIRE(pattern.type == PluginTypeEnum::Recommended);
        REQUIRE(pattern.dependencies.operatorType == OperatorTypeEnum::OR);
        REQUIRE(pattern.dependencies.fileDependencies.size() == 4);
        REQUIRE(pattern.dependencies.flagDependencies.empty());
        REQUIRE(pattern.dependencies.fileDependencies[0].file == "TKDodge.esp");
        REQUIRE(pattern.dependencies.fileDependencies[0].state == FileDependencyTypeEnum::Active);
        REQUIRE(pattern.dependencies.fileDependencies[1].file == "TKDodge.esp");
        REQUIRE(pattern.dependencies.fileDependencies[1].state == FileDependencyTypeEnum::Inactive);
        REQUIRE(pattern.dependencies.fileDependencies[2].file == "UltimateCombat.esp");
        REQUIRE(pattern.dependencies.fileDependencies[2].state == FileDependencyTypeEnum::Active);
        REQUIRE(pattern.dependencies.fileDependencies[3].file == "UltimateCombat.esp");
        REQUIRE(pattern.dependencies.fileDependencies[3].state == FileDependencyTypeEnum::Inactive);

        // Backslash sources are preserved verbatim (FOMOD Plus behavior).
        REQUIRE(tkDodge.files.files[0].source == "Compatibility\\TK Dodge Ultimate Combat");
        REQUIRE(tkDodge.files.files[0].destination == "Nemesis_Engine");
        REQUIRE(tkDodge.files.files[0].priority == 0);
        std::printf("PASS: fomod_module_config — Precision structure\n");
    }

    // Base Object Swapper (test_moduleconf_bos.cpp): version dependency.
    {
        auto config = parse("test_moduleconf_bos.xml");
        const auto& firstPlugin = config.installSteps.installSteps.front()
                                      .optionalFileGroups.groups.front().plugins.plugins.front();
        REQUIRE(firstPlugin.typeDescriptor.dependencyType.defaultType == PluginTypeEnum::Optional);

        const auto& patterns = firstPlugin.typeDescriptor.dependencyType.patterns.patterns;
        REQUIRE(patterns.size() == 3);
        REQUIRE(patterns.front().dependencies.gameDependencies.size() == 1);
        REQUIRE(patterns.front().dependencies.gameDependencies.front().version == "1.6.1130.0");
        std::printf("PASS: fomod_module_config — Base Object Swapper game version\n");
    }

    // Mini Embers (test_moduleconf_miniembers.cpp): nested dependencies.
    {
        auto config = parse("test_moduleconf_miniembers.xml");
        const auto& deps = config.installSteps.installSteps[0]
                               .optionalFileGroups.groups[0].plugins.plugins[0]
                               .typeDescriptor.dependencyType.patterns.patterns[0]
                               .dependencies;
        REQUIRE(deps.flagDependencies.size() == 1);
        REQUIRE(deps.nestedDependencies.size() == 1);
        REQUIRE(deps.nestedDependencies[0].flagDependencies.size() == 2);
        std::printf("PASS: fomod_module_config — Mini Embers nested dependencies\n");
    }

    // Embers (test_moduleconf_embers.cpp): nested dependencies on step 3.
    {
        auto config = parse("test_moduleconf_embers.xml");
        const auto& deps = config.installSteps.installSteps[3]
                               .optionalFileGroups.groups[0].plugins.plugins[0]
                               .typeDescriptor.dependencyType.patterns.patterns[0]
                               .dependencies;
        REQUIRE(deps.flagDependencies.size() == 1);
        REQUIRE(deps.nestedDependencies.size() == 1);
        REQUIRE(deps.nestedDependencies[0].flagDependencies.size() == 2);
        std::printf("PASS: fomod_module_config — Embers nested dependencies\n");
    }
}
