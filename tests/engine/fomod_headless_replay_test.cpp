// Headless FOMOD choice replay: consumes InstallerChoices from a collection
// manifest and replays selections onto a FomodViewModel without UI.
// Source-agnostic - works with any FOMOD whose step/group/plugin names
// match the manifest's recorded selections.

#include "engine/mod/fomod/headless_replay.h"
#include "engine/mod/fomod/fomod_view_model.h"
#include "engine/mod/fomod/module_config.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <catch2/catch_test_macros.hpp>

namespace {

using namespace engine;

std::unique_ptr<ModuleConfiguration> parse_config(const std::string& xml)
{
    const auto path = std::filesystem::temp_directory_path() / "gmm_headless_replay_test.xml";
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

// Helper: find a plugin by name across all steps/groups.
std::shared_ptr<PluginViewModel> find_plugin(
    const std::shared_ptr<FomodViewModel>& vm, const std::string& name)
{
    std::shared_ptr<PluginViewModel> result;
    vm->forEachPlugin([&](GroupRef, PluginRef plugin) {
        if (plugin->getName() == name) {
            result = plugin;
        }
    });
    return result;
}

// Two-step FOMOD with radio (SelectExactlyOne) and checkbox (SelectAny) groups.
const char* kTwoStepXml = R"(<config>
  <moduleName>TestFomod</moduleName>
  <installSteps>
    <installStep name="Body">
      <optionalFileGroups>
        <group name="BodyType" type="SelectExactlyOne">
          <plugins>
            <plugin name="CBBE">
              <typeDescriptor><type>Optional</type></typeDescriptor>
              <files><file source="cbbe.esp"/></files>
            </plugin>
            <plugin name="UNP">
              <typeDescriptor><type>Optional</type></typeDescriptor>
              <files><file source="unp.esp"/></files>
            </plugin>
          </plugins>
        </group>
      </optionalFileGroups>
    </installStep>
    <installStep name="Textures">
      <optionalFileGroups>
        <group name="Resolution" type="SelectAny">
          <plugins>
            <plugin name="2K">
              <typeDescriptor><type>Optional</type></typeDescriptor>
              <files><file source="tex2k.dds"/></files>
            </plugin>
            <plugin name="4K">
              <typeDescriptor><type>Optional</type></typeDescriptor>
              <files><file source="tex4k.dds"/></files>
            </plugin>
          </plugins>
        </group>
        <group name="Style" type="SelectExactlyOne">
          <plugins>
            <plugin name="Vanilla">
              <typeDescriptor><type>Optional</type></typeDescriptor>
              <files><file source="vanilla.dds"/></files>
            </plugin>
            <plugin name="Realistic">
              <typeDescriptor><type>Optional</type></typeDescriptor>
              <files><file source="realistic.dds"/></files>
            </plugin>
          </plugins>
        </group>
      </optionalFileGroups>
    </installStep>
  </installSteps>
</config>)";

} // namespace

// ---------------------------------------------------------------------------
// Happy path: exact match
// ---------------------------------------------------------------------------

TEST_CASE("Headless replay selects named plugins", "[fomod][headless]")
{
    auto vm = make_view_model(kTwoStepXml);

    Collection::InstallerChoices choices;
    choices.type = "fomod";
    choices.selections["Body/BodyType"] = {"UNP"};
    choices.selections["Textures/Resolution"] = {"4K"};
    choices.selections["Textures/Style"] = {"Realistic"};

    const auto result = replay_fomod_choices(*vm, choices);

    REQUIRE(result.success);
    REQUIRE(result.unrecognized_keys.empty());
    REQUIRE(result.unrecognized_options.empty());

    REQUIRE(find_plugin(vm, "UNP")->isSelected());
    REQUIRE_FALSE(find_plugin(vm, "CBBE")->isSelected());
    REQUIRE(find_plugin(vm, "4K")->isSelected());
    // Replay only enables listed plugins; unlisted ones keep their default (off).
    REQUIRE_FALSE(find_plugin(vm, "2K")->isSelected());
    REQUIRE(find_plugin(vm, "Realistic")->isSelected());
    REQUIRE_FALSE(find_plugin(vm, "Vanilla")->isSelected());

    std::printf("PASS: headless_replay — selects named plugins\n");
}

// ---------------------------------------------------------------------------
// Empty selections: defaults remain
// ---------------------------------------------------------------------------

TEST_CASE("Headless replay with empty selections preserves defaults", "[fomod][headless]")
{
    auto vm = make_view_model(kTwoStepXml);

    Collection::InstallerChoices choices;
    choices.type = "fomod";
    // No selections at all

    const auto result = replay_fomod_choices(*vm, choices);

    REQUIRE(result.success);
    REQUIRE(result.unrecognized_keys.empty());
    REQUIRE(result.unrecognized_options.empty());

    // Defaults: the first-step SelectExactlyOne group auto-selects its first
    // Optional plugin (CBBE). Later steps keep plugins off until visited.
    REQUIRE(find_plugin(vm, "CBBE")->isSelected());
    REQUIRE_FALSE(find_plugin(vm, "Vanilla")->isSelected());

    std::printf("PASS: headless_replay — empty selections preserve defaults\n");
}

// ---------------------------------------------------------------------------
// Unrecognized step name
// ---------------------------------------------------------------------------

TEST_CASE("Headless replay reports unrecognized step", "[fomod][headless]")
{
    auto vm = make_view_model(kTwoStepXml);

    Collection::InstallerChoices choices;
    choices.type = "fomod";
    choices.selections["Nonexistent/BodyType"] = {"CBBE"};

    const auto result = replay_fomod_choices(*vm, choices);

    REQUIRE(result.success); // soft failure, not hard
    REQUIRE(result.unrecognized_keys.size() == 1);
    REQUIRE(result.unrecognized_keys[0] == "Nonexistent/BodyType");
    REQUIRE(result.unrecognized_options.empty());

    // CBBE keeps its default (selected) because the replay matched nothing.
    REQUIRE(find_plugin(vm, "CBBE")->isSelected());

    std::printf("PASS: headless_replay — unrecognized step reported\n");
}

// ---------------------------------------------------------------------------
// Unrecognized group name within a valid step
// ---------------------------------------------------------------------------

TEST_CASE("Headless replay reports unrecognized group", "[fomod][headless]")
{
    auto vm = make_view_model(kTwoStepXml);

    Collection::InstallerChoices choices;
    choices.type = "fomod";
    choices.selections["Body/Nonexistent"] = {"CBBE"};

    const auto result = replay_fomod_choices(*vm, choices);

    REQUIRE(result.success);
    REQUIRE(result.unrecognized_keys.size() == 1);
    REQUIRE(result.unrecognized_keys[0] == "Body/Nonexistent");

    std::printf("PASS: headless_replay — unrecognized group reported\n");
}

// ---------------------------------------------------------------------------
// Unrecognized plugin name
// ---------------------------------------------------------------------------

TEST_CASE("Headless replay reports unrecognized plugin", "[fomod][headless]")
{
    auto vm = make_view_model(kTwoStepXml);

    Collection::InstallerChoices choices;
    choices.type = "fomod";
    choices.selections["Body/BodyType"] = {"PhantomSkin"};

    const auto result = replay_fomod_choices(*vm, choices);

    REQUIRE(result.success);
    REQUIRE(result.unrecognized_keys.empty());
    REQUIRE(result.unrecognized_options.size() == 1);
    REQUIRE(result.unrecognized_options[0] == "PhantomSkin");

    // No plugins in the radio group were toggled by the replay
    // (PhantomSkin doesn't exist). CBBE stays at its default (selected).
    REQUIRE(find_plugin(vm, "CBBE")->isSelected());

    std::printf("PASS: headless_replay — unrecognized plugin reported\n");
}

// ---------------------------------------------------------------------------
// Unsupported installer type
// ---------------------------------------------------------------------------

TEST_CASE("Headless replay rejects non-fomod type", "[fomod][headless]")
{
    auto vm = make_view_model(kTwoStepXml);

    Collection::InstallerChoices choices;
    choices.type = "nsis";

    const auto result = replay_fomod_choices(*vm, choices);

    REQUIRE_FALSE(result.success);

    std::printf("PASS: headless_replay — rejects non-fomod type\n");
}

// ---------------------------------------------------------------------------
// Key without slash: treated as step-only lookup (no group match)
// ---------------------------------------------------------------------------

TEST_CASE("Headless replay with slashless key matches step only", "[fomod][headless]")
{
    auto vm = make_view_model(kTwoStepXml);

    Collection::InstallerChoices choices;
    choices.type = "fomod";
    choices.selections["Body"] = {"CBBE"}; // no slash, groupName is empty

    const auto result = replay_fomod_choices(*vm, choices);

    // Empty groupName won't match any group named "" -> unrecognized
    REQUIRE(result.unrecognized_keys.size() == 1);
    REQUIRE(result.unrecognized_keys[0] == "Body");

    std::printf("PASS: headless_replay — slashless key reported as unrecognized\n");
}

// ---------------------------------------------------------------------------
// SelectExactlyOne radio behavior: selecting one deselects the other
// ---------------------------------------------------------------------------

TEST_CASE("Headless replay radio group deselects previous", "[fomod][headless]")
{
    auto vm = make_view_model(kTwoStepXml);

    // Default state: CBBE is auto-selected (first Optional in SelectExactlyOne)
    REQUIRE(find_plugin(vm, "CBBE")->isSelected());

    Collection::InstallerChoices choices;
    choices.type = "fomod";
    choices.selections["Body/BodyType"] = {"UNP"};

    const auto result = replay_fomod_choices(*vm, choices);

    REQUIRE(result.success);
    REQUIRE(find_plugin(vm, "UNP")->isSelected());
    REQUIRE_FALSE(find_plugin(vm, "CBBE")->isSelected()); // radio: deselected

    std::printf("PASS: headless_replay — radio deselects previous\n");
}

// ---------------------------------------------------------------------------
// Multiple selections within the same group (SelectAny)
// ---------------------------------------------------------------------------

TEST_CASE("Headless replay SelectAny allows multiple selections", "[fomod][headless]")
{
    auto vm = make_view_model(kTwoStepXml);

    Collection::InstallerChoices choices;
    choices.type = "fomod";
    choices.selections["Textures/Resolution"] = {"2K", "4K"};

    const auto result = replay_fomod_choices(*vm, choices);

    REQUIRE(result.success);
    REQUIRE(find_plugin(vm, "2K")->isSelected());
    REQUIRE(find_plugin(vm, "4K")->isSelected());

    std::printf("PASS: headless_replay — SelectAny allows multiple\n");
}

// ---------------------------------------------------------------------------
// Single step FOMOD (legacy, no steps)
// ---------------------------------------------------------------------------

const char* kNoStepXml = R"(<config>
  <moduleName>LegacyFomod</moduleName>
</config>)";

TEST_CASE("Headless replay on stepless FOMOD returns empty diagnostics", "[fomod][headless]")
{
    auto vm = make_view_model(kNoStepXml);

    Collection::InstallerChoices choices;
    choices.type = "fomod";
    choices.selections["Anything/Gone"] = {"Nope"};

    const auto result = replay_fomod_choices(*vm, choices);

    REQUIRE(result.success);
    REQUIRE(result.unrecognized_keys.size() == 1);

    std::printf("PASS: headless_replay — stepless FOMOD\n");
}

// ---------------------------------------------------------------------------
// Partial match: some keys match, some don't
// ---------------------------------------------------------------------------

TEST_CASE("Headless replay partial match reports only mismatches", "[fomod][headless]")
{
    auto vm = make_view_model(kTwoStepXml);

    Collection::InstallerChoices choices;
    choices.type = "fomod";
    choices.selections["Body/BodyType"] = {"UNP"};
    choices.selections["Mystery/Unknown"] = {"Something"};

    const auto result = replay_fomod_choices(*vm, choices);

    REQUIRE(result.success);
    REQUIRE(result.unrecognized_keys.size() == 1);
    REQUIRE(result.unrecognized_keys[0] == "Mystery/Unknown");
    REQUIRE(result.unrecognized_options.empty());

    // UNP was still selected despite the other key failing
    REQUIRE(find_plugin(vm, "UNP")->isSelected());

    std::printf("PASS: headless_replay — partial match reports only mismatches\n");
}

// ---------------------------------------------------------------------------
// Idempotency: replaying twice doesn't change state
// ---------------------------------------------------------------------------

TEST_CASE("Headless replay is idempotent", "[fomod][headless]")
{
    auto vm = make_view_model(kTwoStepXml);

    Collection::InstallerChoices choices;
    choices.type = "fomod";
    choices.selections["Body/BodyType"] = {"UNP"};
    choices.selections["Textures/Resolution"] = {"4K"};

    const auto r1 = replay_fomod_choices(*vm, choices);
    const auto r2 = replay_fomod_choices(*vm, choices);

    REQUIRE(r1.success);
    REQUIRE(r2.success);
    REQUIRE(r1.unrecognized_keys == r2.unrecognized_keys);
    REQUIRE(r1.unrecognized_options == r2.unrecognized_options);

    // State is the same after two runs
    REQUIRE(find_plugin(vm, "UNP")->isSelected());
    REQUIRE(find_plugin(vm, "4K")->isSelected());

    std::printf("PASS: headless_replay — idempotent\n");
}

// ---------------------------------------------------------------------------
// Choice groups (manifest-level "pick one of these mods")
// ---------------------------------------------------------------------------

namespace {

Collection::ChoiceGroup make_group(const std::string& id, Collection::ChoiceMode mode,
    std::vector<std::string> members)
{
    Collection::ChoiceGroup group;
    group.id = id;
    group.name = id;
    group.mode = mode;
    group.member_mod_ids = std::move(members);
    return group;
}

} // namespace

TEST_CASE("Choice groups exactly-one happy path", "[fomod][headless][choices]")
{
    const std::vector<Collection::ChoiceGroup> groups = {
        make_group("texture-pack", Collection::ChoiceMode::ExactlyOne, {"smi", "sky202x"}),
    };

    const auto result = resolve_choice_groups(groups, {{"texture-pack", {"sky202x"}}});

    REQUIRE(result.success);
    REQUIRE(result.selected_mod_ids == std::vector<std::string>{"sky202x"});
    REQUIRE(result.unrecognized_groups.empty());
    REQUIRE(result.unrecognized_members.empty());
    REQUIRE(result.mode_violations.empty());

    std::printf("PASS: headless_replay — choice exactly-one happy path\n");
}

TEST_CASE("Choice groups exactly-one empty pick is a violation", "[fomod][headless][choices]")
{
    const std::vector<Collection::ChoiceGroup> groups = {
        make_group("texture-pack", Collection::ChoiceMode::ExactlyOne, {"smi", "sky202x"}),
    };

    const auto result = resolve_choice_groups(groups, {});

    REQUIRE_FALSE(result.success);
    REQUIRE(result.selected_mod_ids.empty());
    REQUIRE(result.mode_violations == std::vector<std::string>{"texture-pack"});

    std::printf("PASS: headless_replay — choice exactly-one empty violation\n");
}

TEST_CASE("Choice groups exactly-one double pick keeps first", "[fomod][headless][choices]")
{
    const std::vector<Collection::ChoiceGroup> groups = {
        make_group("texture-pack", Collection::ChoiceMode::ExactlyOne, {"smi", "sky202x"}),
    };

    const auto result = resolve_choice_groups(groups, {{"texture-pack", {"smi", "sky202x"}}});

    REQUIRE_FALSE(result.success);
    REQUIRE(result.selected_mod_ids == std::vector<std::string>{"smi"});
    REQUIRE(result.mode_violations == std::vector<std::string>{"texture-pack"});

    std::printf("PASS: headless_replay — choice exactly-one double pick\n");
}

TEST_CASE("Choice groups at-most-one allows empty, rejects double",
    "[fomod][headless][choices]")
{
    const std::vector<Collection::ChoiceGroup> groups = {
        make_group("enb", Collection::ChoiceMode::AtMostOne, {"enb-a", "enb-b"}),
    };

    const auto empty = resolve_choice_groups(groups, {});
    REQUIRE(empty.success);
    REQUIRE(empty.selected_mod_ids.empty());

    const auto doubled = resolve_choice_groups(groups, {{"enb", {"enb-a", "enb-b"}}});
    REQUIRE_FALSE(doubled.success);
    REQUIRE(doubled.selected_mod_ids == std::vector<std::string>{"enb-a"});
    REQUIRE(doubled.mode_violations == std::vector<std::string>{"enb"});

    std::printf("PASS: headless_replay — choice at-most-one\n");
}

TEST_CASE("Choice groups report unknown group and member", "[fomod][headless][choices]")
{
    const std::vector<Collection::ChoiceGroup> groups = {
        make_group("texture-pack", Collection::ChoiceMode::ExactlyOne, {"smi", "sky202x"}),
    };

    const auto result = resolve_choice_groups(
        groups, {{"texture-pack", {"phantom"}}, {"mystery", {"smi"}}});

    REQUIRE(result.selected_mod_ids.empty()); // "phantom" is not a member
    REQUIRE(result.unrecognized_groups == std::vector<std::string>{"mystery"});
    REQUIRE(result.unrecognized_members == std::vector<std::string>{"phantom"});
    // No valid pick for an exactly-one group -> violation still recorded.
    REQUIRE(result.mode_violations == std::vector<std::string>{"texture-pack"});
    REQUIRE_FALSE(result.success);

    std::printf("PASS: headless_replay — choice unknown group/member\n");
}
