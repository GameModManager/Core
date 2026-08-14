// FOMOD condition semantics: file-state/game-version seams, AND/OR composite
// dependencies (no short-circuit, matching MO2), most-recent-wins flag
// ordering, first-match type patterns, empty-value flag deps (flag unset),
// and step visibility. Ported from FOMOD Plus (MIT) semantics.

#include "engine/mod/fomod/condition_tester.h"
#include "engine/mod/fomod/fomod_view_model.h"
#include "engine/mod/fomod/module_config.h"
#include "engine/mod/fomod/view_models.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <catch2/catch_test_macros.hpp>

namespace {

using namespace engine;

std::unique_ptr<ModuleConfiguration> parse_config(const std::string& xml)
{
    const auto path = std::filesystem::temp_directory_path() / "gmm_fomod_condition_test.xml";
    std::ofstream(path) << xml;
    auto config = std::make_unique<ModuleConfiguration>();
    const bool parsed = config->deserialize(path);
    REQUIRE(parsed);
    std::filesystem::remove(path);
    return config;
}

std::shared_ptr<FomodViewModel> make_view_model(const std::string& xml)
{
    return FomodViewModel::create(nullptr, nullptr, parse_config(xml), nullptr);
}

std::shared_ptr<PluginViewModel> find_plugin(const std::shared_ptr<FomodViewModel>& vm, const std::string& name)
{
    std::shared_ptr<PluginViewModel> result;
    vm->forEachPlugin([&](GroupRef, PluginRef plugin) {
        if (plugin->getName() == name) {
            result = plugin;
        }
    });
    REQUIRE(result);
    return result;
}

std::shared_ptr<GroupViewModel> first_group(const std::shared_ptr<FomodViewModel>& vm, int stepIndex)
{
    return vm->getSteps()[stepIndex]->getGroups().front();
}

class CountingResolver : public FomodFileStateResolver {
public:
    std::unordered_map<std::string, FileDependencyTypeEnum> states;
    int calls = 0;
    FileDependencyTypeEnum file_state(const std::string& name) override
    {
        ++calls;
        if (states.count(name)) {
            return states.at(name);
        }
        return FileDependencyTypeEnum::Missing;
    }
};

class StubVersion : public FomodGameVersionProvider {
public:
    std::string version;
    std::string game_version() override { return version; }
};

void test_file_dependencies_and_cache()
{
    CountingResolver resolver;
    resolver.states["Patch.esp"] = FileDependencyTypeEnum::Active;
    FomodConditionTester tester(&resolver, nullptr);

    FileDependency dep;
    dep.file = "Patch.esp";
    dep.state = FileDependencyTypeEnum::Active;
    REQUIRE(tester.testFileDependency(dep));
    dep.state = FileDependencyTypeEnum::Inactive;
    REQUIRE(!tester.testFileDependency(dep));
    dep.state = FileDependencyTypeEnum::Missing;
    REQUIRE(!tester.testFileDependency(dep));
    REQUIRE(resolver.calls == 1);  // cached after the first resolution

    FileDependency missing;
    missing.file = "Nope.esp";
    missing.state = FileDependencyTypeEnum::Missing;
    REQUIRE(tester.testFileDependency(missing));
    REQUIRE(tester.testFileDependency(missing));
    REQUIRE(resolver.calls == 2);  // second miss resolved once, then cached
    std::printf("PASS: fomod_condition — file states + resolver cache\n");
}

void test_game_version_lexicographic()
{
    StubVersion version;
    version.version = "1.6.640";
    FomodConditionTester tester(nullptr, &version);

    GameDependency dep;
    dep.version = "1.5.97";
    REQUIRE(tester.testGameDependency(dep));
    dep.version = "1.6.640";
    REQUIRE(tester.testGameDependency(dep));
    dep.version = "1.7.0";
    REQUIRE(!tester.testGameDependency(dep));
    std::printf("PASS: fomod_condition — game version lexicographic comparison\n");
}

void test_null_provider_passes()
{
    FomodConditionTester tester(nullptr, nullptr);
    GameDependency dep;
    dep.version = "9.9.9";
    REQUIRE(tester.testGameDependency(dep));  // null provider → satisfied + warning
    std::printf("PASS: fomod_condition — null game-version provider passes\n");
}

// A plugin whose type descriptor is driven by a flag set by an earlier
// plugin's selection (setFlagForPluginState on toggle).
void test_flag_dependency_drives_type()
{
    const std::string xml = R"(<config>
  <moduleName>Cond</moduleName>
  <installSteps>
    <installStep name="S1">
      <optionalFileGroups>
        <group name="G" type="SelectAny">
          <plugins>
            <plugin name="Toggle">
              <typeDescriptor><dependencyType><defaultType name="Optional"/></dependencyType></typeDescriptor>
              <conditionFlags><flag name="f">on</flag></conditionFlags>
              <files/>
            </plugin>
          </plugins>
        </group>
      </optionalFileGroups>
    </installStep>
    <installStep name="S2">
      <optionalFileGroups>
        <group name="G2" type="SelectAny">
          <plugins>
            <plugin name="Follow">
              <typeDescriptor>
                <dependencyType>
                  <defaultType name="Optional"/>
                  <patterns>
                    <pattern>
                      <type name="Recommended"/>
                      <dependencies operator="And">
                        <flagDependency flag="f" value="on"/>
                      </dependencies>
                    </pattern>
                  </patterns>
                </dependencyType>
              </typeDescriptor>
              <files/>
            </plugin>
          </plugins>
        </group>
      </optionalFileGroups>
    </installStep>
  </installSteps>
</config>)";

    auto vm = make_view_model(xml);
    auto toggle = find_plugin(vm, "Toggle");
    auto follow = find_plugin(vm, "Follow");
    auto group = first_group(vm, 0);

    REQUIRE(!toggle->isSelected());
    REQUIRE(!follow->isSelected());
    REQUIRE(follow->getCurrentPluginType() == PluginTypeEnum::Optional);

    vm->togglePlugin(group, toggle, true);
    REQUIRE(toggle->isSelected());
    REQUIRE(follow->isSelected());
    REQUIRE(follow->getCurrentPluginType() == PluginTypeEnum::Recommended);

    vm->togglePlugin(group, toggle, false);
    REQUIRE(!toggle->isSelected());
    REQUIRE(!follow->isSelected());
    REQUIRE(follow->getCurrentPluginType() == PluginTypeEnum::Optional);
    std::printf("PASS: fomod_condition — flag dependency drives plugin type\n");
}

// Two plugins in different steps set the same flag to different values. The
// later step wins, so a plugin conditioned on the later value flips on.
void test_most_recent_flag_wins()
{
    const std::string xml = R"(<config>
  <moduleName>MRW</moduleName>
  <installSteps>
    <installStep name="S1">
      <optionalFileGroups>
        <group name="G" type="SelectAny">
          <plugins>
            <plugin name="Early">
              <typeDescriptor><dependencyType><defaultType name="Optional"/></dependencyType></typeDescriptor>
              <conditionFlags><flag name="m">early</flag></conditionFlags>
              <files/>
            </plugin>
          </plugins>
        </group>
      </optionalFileGroups>
    </installStep>
    <installStep name="S2">
      <optionalFileGroups>
        <group name="G2" type="SelectAny">
          <plugins>
            <plugin name="Late">
              <typeDescriptor><dependencyType><defaultType name="Optional"/></dependencyType></typeDescriptor>
              <conditionFlags><flag name="m">late</flag></conditionFlags>
              <files/>
            </plugin>
          </plugins>
        </group>
      </optionalFileGroups>
    </installStep>
    <installStep name="S3">
      <optionalFileGroups>
        <group name="G3" type="SelectAny">
          <plugins>
            <plugin name="Follower">
              <typeDescriptor>
                <dependencyType>
                  <defaultType name="Optional"/>
                  <patterns>
                    <pattern>
                      <type name="Recommended"/>
                      <dependencies operator="And">
                        <flagDependency flag="m" value="late"/>
                      </dependencies>
                    </pattern>
                  </patterns>
                </dependencyType>
              </typeDescriptor>
              <files/>
            </plugin>
          </plugins>
        </group>
      </optionalFileGroups>
    </installStep>
  </installSteps>
</config>)";

    auto vm = make_view_model(xml);
    auto early = find_plugin(vm, "Early");
    auto late = find_plugin(vm, "Late");
    auto follower = find_plugin(vm, "Follower");
    auto groupEarly = first_group(vm, 0);
    auto groupLate = first_group(vm, 1);

    vm->togglePlugin(groupEarly, early, true);
    REQUIRE(follower->getCurrentPluginType() == PluginTypeEnum::Optional);  // only "early" is set

    vm->togglePlugin(groupLate, late, true);
    REQUIRE(follower->getCurrentPluginType() == PluginTypeEnum::Recommended);
    REQUIRE(follower->isSelected());

    // Most-recent-wins ordering in the flag map: step 2 before step 1.
    const auto flags = vm->flag_map()->getFlagsByKey("m");
    REQUIRE(flags.size() == 2);
    REQUIRE(flags[0].second == "late");
    REQUIRE(flags[1].second == "early");

    vm->togglePlugin(groupEarly, early, false);
    REQUIRE(follower->isSelected());  // still "late", unaffected by removing "early"

    vm->togglePlugin(groupLate, late, false);
    REQUIRE(!follower->isSelected());
    REQUIRE(follower->getCurrentPluginType() == PluginTypeEnum::Optional);
    std::printf("PASS: fomod_condition — most-recent-wins flag ordering\n");
}

// Patterns are first-match-wins: with both flags set, the first pattern's
// type wins even though the second pattern also matches.
void test_first_match_pattern_wins()
{
    const std::string xml = R"(<config>
  <moduleName>FM</moduleName>
  <installSteps>
    <installStep name="S1">
      <optionalFileGroups>
        <group name="G" type="SelectAny">
          <plugins>
            <plugin name="SetA1">
              <typeDescriptor><dependencyType><defaultType name="Optional"/></dependencyType></typeDescriptor>
              <conditionFlags><flag name="a">1</flag></conditionFlags>
              <files/>
            </plugin>
            <plugin name="SetA2">
              <typeDescriptor><dependencyType><defaultType name="Optional"/></dependencyType></typeDescriptor>
              <conditionFlags><flag name="a">2</flag></conditionFlags>
              <files/>
            </plugin>
          </plugins>
        </group>
      </optionalFileGroups>
    </installStep>
    <installStep name="S2">
      <optionalFileGroups>
        <group name="G2" type="SelectAny">
          <plugins>
            <plugin name="Dual">
              <typeDescriptor>
                <dependencyType>
                  <defaultType name="Optional"/>
                  <patterns>
                    <pattern>
                      <type name="Recommended"/>
                      <dependencies operator="And"><flagDependency flag="a" value="1"/></dependencies>
                    </pattern>
                    <pattern>
                      <type name="Required"/>
                      <dependencies operator="And"><flagDependency flag="a" value="2"/></dependencies>
                    </pattern>
                  </patterns>
                </dependencyType>
              </typeDescriptor>
              <files/>
            </plugin>
          </plugins>
        </group>
      </optionalFileGroups>
    </installStep>
  </installSteps>
</config>)";

    auto vm = make_view_model(xml);
    auto setA1 = find_plugin(vm, "SetA1");
    auto setA2 = find_plugin(vm, "SetA2");
    auto dual = find_plugin(vm, "Dual");
    auto group = first_group(vm, 0);

    vm->togglePlugin(group, setA1, true);
    REQUIRE(dual->getCurrentPluginType() == PluginTypeEnum::Recommended);

    vm->togglePlugin(group, setA2, true);
    REQUIRE(dual->getCurrentPluginType() == PluginTypeEnum::Recommended);  // first pattern still wins

    vm->togglePlugin(group, setA1, false);
    REQUIRE(dual->getCurrentPluginType() == PluginTypeEnum::Required);  // only second pattern matches
    REQUIRE(dual->isSelected());
    REQUIRE(!dual->isEnabled());
    std::printf("PASS: fomod_condition — first-match pattern wins\n");
}

// An empty-value flag dependency matches when the flag is NOT set.
void test_empty_value_flag_dependency()
{
    const std::string xml = R"(<config>
  <moduleName>EV</moduleName>
  <installSteps>
    <installStep name="S1">
      <optionalFileGroups>
        <group name="G" type="SelectAny">
          <plugins>
            <plugin name="SetsU">
              <typeDescriptor><dependencyType><defaultType name="Optional"/></dependencyType></typeDescriptor>
              <conditionFlags><flag name="u">on</flag></conditionFlags>
              <files/>
            </plugin>
          </plugins>
        </group>
      </optionalFileGroups>
    </installStep>
    <installStep name="S2">
      <optionalFileGroups>
        <group name="G2" type="SelectAny">
          <plugins>
            <plugin name="NoFlag">
              <typeDescriptor>
                <dependencyType>
                  <defaultType name="Optional"/>
                  <patterns>
                    <pattern>
                      <type name="Recommended"/>
                      <dependencies operator="And"><flagDependency flag="u" value=""/></dependencies>
                    </pattern>
                  </patterns>
                </dependencyType>
              </typeDescriptor>
              <files/>
            </plugin>
          </plugins>
        </group>
      </optionalFileGroups>
    </installStep>
  </installSteps>
</config>)";

    auto vm = make_view_model(xml);
    auto setsU = find_plugin(vm, "SetsU");
    auto noFlag = find_plugin(vm, "NoFlag");
    auto group = first_group(vm, 0);

    REQUIRE(noFlag->isSelected());  // "u" unset at start → Recommended
    REQUIRE(noFlag->getCurrentPluginType() == PluginTypeEnum::Recommended);

    vm->togglePlugin(group, setsU, true);
    REQUIRE(!noFlag->isSelected());
    REQUIRE(noFlag->getCurrentPluginType() == PluginTypeEnum::Optional);

    vm->togglePlugin(group, setsU, false);
    REQUIRE(noFlag->isSelected());
    REQUIRE(noFlag->getCurrentPluginType() == PluginTypeEnum::Recommended);
    std::printf("PASS: fomod_condition — empty-value flag dependency (unset)\n");
}

// AND semantics: all flags must be set for the pattern to match.
void test_and_semantics()
{
    const std::string xml = R"(<config>
  <moduleName>AND</moduleName>
  <installSteps>
    <installStep name="S1">
      <optionalFileGroups>
        <group name="G" type="SelectAny">
          <plugins>
            <plugin name="SetP">
              <typeDescriptor><dependencyType><defaultType name="Optional"/></dependencyType></typeDescriptor>
              <conditionFlags><flag name="p">1</flag></conditionFlags>
              <files/>
            </plugin>
            <plugin name="SetQ">
              <typeDescriptor><dependencyType><defaultType name="Optional"/></dependencyType></typeDescriptor>
              <conditionFlags><flag name="q">2</flag></conditionFlags>
              <files/>
            </plugin>
          </plugins>
        </group>
      </optionalFileGroups>
    </installStep>
    <installStep name="S2">
      <optionalFileGroups>
        <group name="G2" type="SelectAny">
          <plugins>
            <plugin name="AndPlugin">
              <typeDescriptor>
                <dependencyType>
                  <defaultType name="Optional"/>
                  <patterns>
                    <pattern>
                      <type name="Recommended"/>
                      <dependencies operator="And">
                        <flagDependency flag="p" value="1"/>
                        <flagDependency flag="q" value="2"/>
                      </dependencies>
                    </pattern>
                  </patterns>
                </dependencyType>
              </typeDescriptor>
              <files/>
            </plugin>
          </plugins>
        </group>
      </optionalFileGroups>
    </installStep>
  </installSteps>
</config>)";

    auto vm = make_view_model(xml);
    auto setP = find_plugin(vm, "SetP");
    auto setQ = find_plugin(vm, "SetQ");
    auto andPlugin = find_plugin(vm, "AndPlugin");
    auto group = first_group(vm, 0);

    vm->togglePlugin(group, setP, true);
    REQUIRE(andPlugin->getCurrentPluginType() == PluginTypeEnum::Optional);  // only p set

    vm->togglePlugin(group, setQ, true);
    REQUIRE(andPlugin->getCurrentPluginType() == PluginTypeEnum::Recommended);
    REQUIRE(andPlugin->isSelected());

    vm->togglePlugin(group, setQ, false);
    REQUIRE(andPlugin->getCurrentPluginType() == PluginTypeEnum::Optional);
    REQUIRE(!andPlugin->isSelected());
    std::printf("PASS: fomod_condition — AND composite dependency\n");
}

// OR semantics: any matching flag satisfies the dependency.
void test_or_semantics()
{
    const std::string xml = R"(<config>
  <moduleName>OR</moduleName>
  <installSteps>
    <installStep name="S1">
      <optionalFileGroups>
        <group name="G" type="SelectAny">
          <plugins>
            <plugin name="SetX">
              <typeDescriptor><dependencyType><defaultType name="Optional"/></dependencyType></typeDescriptor>
              <conditionFlags><flag name="x">1</flag></conditionFlags>
              <files/>
            </plugin>
            <plugin name="SetY">
              <typeDescriptor><dependencyType><defaultType name="Optional"/></dependencyType></typeDescriptor>
              <conditionFlags><flag name="y">2</flag></conditionFlags>
              <files/>
            </plugin>
          </plugins>
        </group>
      </optionalFileGroups>
    </installStep>
    <installStep name="S2">
      <optionalFileGroups>
        <group name="G2" type="SelectAny">
          <plugins>
            <plugin name="OrPlugin">
              <typeDescriptor>
                <dependencyType>
                  <defaultType name="Optional"/>
                  <patterns>
                    <pattern>
                      <type name="Recommended"/>
                      <dependencies operator="Or">
                        <flagDependency flag="x" value="1"/>
                        <flagDependency flag="y" value="2"/>
                      </dependencies>
                    </pattern>
                  </patterns>
                </dependencyType>
              </typeDescriptor>
              <files/>
            </plugin>
          </plugins>
        </group>
      </optionalFileGroups>
    </installStep>
  </installSteps>
</config>)";

    auto vm = make_view_model(xml);
    auto setX = find_plugin(vm, "SetX");
    auto setY = find_plugin(vm, "SetY");
    auto orPlugin = find_plugin(vm, "OrPlugin");
    auto group = first_group(vm, 0);

    vm->togglePlugin(group, setX, true);
    REQUIRE(orPlugin->getCurrentPluginType() == PluginTypeEnum::Recommended);
    REQUIRE(orPlugin->isSelected());

    vm->togglePlugin(group, setY, true);
    REQUIRE(orPlugin->isSelected());  // still selected, both flags now set

    vm->togglePlugin(group, setX, false);
    REQUIRE(orPlugin->isSelected());  // y still matches

    vm->togglePlugin(group, setY, false);
    REQUIRE(orPlugin->getCurrentPluginType() == PluginTypeEnum::Optional);
    REQUIRE(!orPlugin->isSelected());
    std::printf("PASS: fomod_condition — OR composite dependency\n");
}

// A step with a visibility condition disappears until an earlier plugin sets
// the flag; step navigation skips hidden steps.
void test_step_visibility()
{
    const std::string xml = R"(<config>
  <moduleName>VIS</moduleName>
  <installSteps>
    <installStep name="S1">
      <optionalFileGroups>
        <group name="G" type="SelectAny">
          <plugins>
            <plugin name="Reveal">
              <typeDescriptor><dependencyType><defaultType name="Optional"/></dependencyType></typeDescriptor>
              <conditionFlags><flag name="show">on</flag></conditionFlags>
              <files/>
            </plugin>
          </plugins>
        </group>
      </optionalFileGroups>
    </installStep>
    <installStep name="S2">
      <visible><flagDependency flag="show" value="on"/></visible>
      <optionalFileGroups>
        <group name="G2" type="SelectAny">
          <plugins>
            <plugin name="Hidden">
              <typeDescriptor><dependencyType><defaultType name="Optional"/></dependencyType></typeDescriptor>
              <files/>
            </plugin>
          </plugins>
        </group>
      </optionalFileGroups>
    </installStep>
    <installStep name="S3">
      <optionalFileGroups>
        <group name="G3" type="SelectAny">
          <plugins>
            <plugin name="Always">
              <typeDescriptor><dependencyType><defaultType name="Optional"/></dependencyType></typeDescriptor>
              <files/>
            </plugin>
          </plugins>
        </group>
      </optionalFileGroups>
    </installStep>
  </installSteps>
</config>)";

    auto vm = make_view_model(xml);
    auto reveal = find_plugin(vm, "Reveal");
    auto group = first_group(vm, 0);

    // S2 hidden initially: stepForward skips it.
    vm->stepForward();
    REQUIRE(vm->getActiveStep()->getName() == "S3");
    REQUIRE(vm->isLastVisibleStep());

    // Setting the flag makes S2 visible; stepBack now lands on it.
    vm->togglePlugin(group, reveal, true);
    vm->stepBack();
    REQUIRE(vm->getActiveStep()->getName() == "S2");

    std::printf("PASS: fomod_condition — step visibility\n");
}

}  // namespace

TEST_CASE("fomod condition", "[engine]") {
    test_file_dependencies_and_cache();
    test_game_version_lexicographic();
    test_null_provider_passes();
    test_flag_dependency_drives_type();
    test_most_recent_flag_wins();
    test_first_match_pattern_wins();
    test_empty_value_flag_dependency();
    test_and_semantics();
    test_or_semantics();
    test_step_visibility();
}
