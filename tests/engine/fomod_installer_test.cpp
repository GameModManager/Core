// FomodFileInstaller: priority-sorted copy (ascending, XML-order tie-break,
// later wins on overwrite), destination remap, folder-children copy (empty
// destination = install root), missing-file collection, path-traversal guard,
// staging-dir swap, and fomod.json choice serialization.

#include "engine/mod/fomod/file_installer.h"
#include "engine/mod/fomod/fomod_view_model.h"
#include "engine/mod/fomod/module_config.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>

namespace {

using namespace engine;
namespace fs = std::filesystem;

struct TestDir {
    fs::path root;       // contains staging/
    fs::path staging;    // the mod root passed to the installer
    explicit TestDir(const std::string& tag)
    {
        root = fs::temp_directory_path() / ("gmm_fomod_install_" + tag);
        fs::remove_all(root);
        staging = root / "staging";
        fs::create_directories(staging / "fomod");
    }
    ~TestDir() { fs::remove_all(root); }
};

void write_file(const fs::path& path, const std::string& content)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path) << content;
}

std::string read_file(const fs::path& path)
{
    std::ifstream in(path);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return content;
}

std::unique_ptr<ModuleConfiguration> parse_config(const std::string& xml)
{
    const auto path = fs::temp_directory_path() / "gmm_fomod_install_cfg.xml";
    std::ofstream(path) << xml;
    auto config = std::make_unique<ModuleConfiguration>();
    const bool parsed = config->deserialize(path);
    REQUIRE(parsed);
    fs::remove(path);
    return config;
}

// Parses the config and builds a fully-initialized view model. Recommended
// plugins auto-select during create, matching the wizard's initial state.
std::shared_ptr<FomodViewModel> view_model(const std::string& xml)
{
    return FomodViewModel::create(nullptr, nullptr, parse_config(xml), nullptr);
}

void test_priority_remap_and_folders()
{
    TestDir dir("priority");
    const auto& staging = dir.staging;

    write_file(staging / "fomod/ModuleConfig.xml", "<config/>");
    write_file(staging / "meshes/a.nif", "A");
    write_file(staging / "meshes/b.nif", "B");
    write_file(staging / "textures/t1.dds", "T1");
    write_file(staging / "textures/sub/t2.dds", "T2");
    write_file(staging / "overwrite_low.txt", "LOW");
    write_file(staging / "overwrite_high.txt", "HIGH");
    write_file(staging / "same_first.txt", "FIRST");
    write_file(staging / "same_second.txt", "SECOND");

    const std::string xml = R"(<config>
  <moduleName>InstallTest</moduleName>
  <requiredInstallFiles>
    <file source="meshes/a.nif" priority="1"/>
    <file source="overwrite_low.txt" destination="out.txt" priority="1"/>
    <file source="overwrite_high.txt" destination="out.txt" priority="2"/>
    <file source="same_first.txt" destination="same.txt" priority="3"/>
    <file source="same_second.txt" destination="same.txt" priority="3"/>
    <folder source="textures" destination="Data/textures" priority="0"/>
    <file source="does_not_exist.nif" priority="0"/>
  </requiredInstallFiles>
  <installSteps>
    <installStep name="S1">
      <optionalFileGroups>
        <group name="G" type="SelectAny">
          <plugins>
            <plugin name="Extra">
              <typeDescriptor>
                <dependencyType>
                  <defaultType name="Recommended"/>
                  <patterns/>
                </dependencyType>
              </typeDescriptor>
              <files><file source="meshes/b.nif" priority="0"/></files>
            </plugin>
          </plugins>
        </group>
      </optionalFileGroups>
    </installStep>
  </installSteps>
</config>)";

    auto vm = view_model(xml);
    FomodFileInstaller installer(staging, vm);
    std::vector<std::string> missing;
    const bool applied = installer.apply(&missing);
    REQUIRE(applied);

    // Destination remap + higher priority overwrites lower.
    REQUIRE(fs::exists(staging / "out.txt"));
    REQUIRE(read_file(staging / "out.txt") == "HIGH");

    // Equal priority: XML order wins (stable sort).
    REQUIRE(read_file(staging / "same.txt") == "SECOND");

    // Plain files land at the tree root with their source-relative path.
    REQUIRE(fs::exists(staging / "meshes/a.nif"));
    REQUIRE(fs::exists(staging / "meshes/b.nif"));

    // Folder children copied under the destination prefix.
    REQUIRE(fs::exists(staging / "Data/textures/t1.dds"));
    REQUIRE(fs::exists(staging / "Data/textures/sub/t2.dds"));

    // The fomod/ dir is gone; original files that were only sources are gone.
    REQUIRE(!fs::exists(staging / "fomod"));
    REQUIRE(!fs::exists(staging / "overwrite_low.txt"));
    REQUIRE(!fs::exists(staging / "textures"));

    // Missing sources are reported, not fatal.
    REQUIRE(missing.size() == 1);
    REQUIRE(missing[0] == "does_not_exist.nif");

    // No leftover sibling install tree.
    REQUIRE(!fs::exists(staging.string() + "_gmm_fomod_install"));
    std::printf("PASS: fomod_installer — priority, remap, folders, missing\n");
}

void test_folder_empty_destination()
{
    TestDir dir("rootfolder");
    const auto& staging = dir.staging;
    write_file(staging / "fomod/ModuleConfig.xml", "<config/>");
    write_file(staging / "textures/t1.dds", "T1");
    write_file(staging / "textures/sub/t2.dds", "T2");

    const std::string xml = R"(<config>
  <moduleName>RootFolder</moduleName>
  <requiredInstallFiles>
    <folder source="textures" destination="" priority="0"/>
  </requiredInstallFiles>
  <installSteps/>
</config>)";

    auto vm = view_model(xml);
    FomodFileInstaller installer(staging, vm);
    std::vector<std::string> missing;
    const bool applied = installer.apply(&missing);
    REQUIRE(applied);
    REQUIRE(missing.empty());

    // Empty destination puts the folder's children at the install root.
    REQUIRE(fs::exists(staging / "t1.dds"));
    REQUIRE(fs::exists(staging / "sub/t2.dds"));
    REQUIRE(!fs::exists(staging / "textures"));
    std::printf("PASS: fomod_installer — folder with empty destination\n");
}

void test_path_traversal_guard()
{
    TestDir dir("traversal");
    const auto& staging = dir.staging;
    write_file(staging / "fomod/ModuleConfig.xml", "<config/>");
    write_file(staging / "meshes/ok.nif", "OK");

    const std::string xml = R"(<config>
  <moduleName>Traversal</moduleName>
  <requiredInstallFiles>
    <file source="../escape.nif" priority="0"/>
    <file source="meshes/ok.nif" destination="../../evil/ok.nif" priority="0"/>
  </requiredInstallFiles>
  <installSteps/>
</config>)";

    auto vm = view_model(xml);
    FomodFileInstaller installer(staging, vm);
    std::vector<std::string> missing;
    const bool applied = installer.apply(&missing);
    REQUIRE(applied);

    // Both entries skipped; nothing escaped the mod root. The final install
    // tree is empty because every entry was rejected.
    REQUIRE(missing.empty());
    REQUIRE(!fs::exists(staging / "ok.nif"));
    REQUIRE(!fs::exists(dir.root / "evil"));
    REQUIRE(fs::exists(staging / "meshes") == false);
    std::printf("PASS: fomod_installer — path traversal guard\n");
}

void test_generate_fomod_json()
{
    TestDir dir("json");
    const auto& staging = dir.staging;
    write_file(staging / "fomod/ModuleConfig.xml", "<config/>");

    const std::string xml = R"(<config>
  <moduleName>Json</moduleName>
  <installSteps>
    <installStep name="StepA">
      <optionalFileGroups>
        <group name="GroupA" type="SelectAny">
          <plugins>
            <plugin name="Chosen">
              <typeDescriptor>
                <dependencyType>
                  <defaultType name="Recommended"/>
                  <patterns/>
                </dependencyType>
              </typeDescriptor>
              <files/>
            </plugin>
            <plugin name="SkipMe">
              <typeDescriptor>
                <dependencyType>
                  <defaultType name="Optional"/>
                  <patterns/>
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

    auto vm = view_model(xml);
    auto chosen = std::shared_ptr<PluginViewModel>();
    auto skipMe = std::shared_ptr<PluginViewModel>();
    vm->forEachPlugin([&](GroupRef, PluginRef plugin) {
        if (plugin->getName() == "Chosen") {
            chosen = plugin;
        }
        if (plugin->getName() == "SkipMe") {
            skipMe = plugin;
        }
    });
    REQUIRE(chosen->isSelected());  // auto-selected Recommended
    REQUIRE(!skipMe->isSelected());
    FomodViewModel::markManuallySet(skipMe);

    FomodFileInstaller installer(staging, vm);
    const auto json = nlohmann::json::parse(installer.generateFomodJson());
    const auto& group = json["steps"][0]["groups"][0];
    REQUIRE(group["plugins"].size() == 1);
    REQUIRE(group["plugins"][0] == "Chosen");
    REQUIRE(group["deselected"].size() == 1);
    REQUIRE(group["deselected"][0] == "SkipMe");
    std::printf("PASS: fomod_installer — generateFomodJson\n");
}

void test_windows_paths_case_insensitive()
{
    TestDir dir("winpaths");
    const auto& staging = dir.staging;
    write_file(staging / "fomod/ModuleConfig.xml", "<config/>");

    // On-disk tree uses forward slashes and real casing; the FOMOD references
    // the same files with backslash separators and different case.
    write_file(staging / "Skeleton Rig/HDT/body.nif", "BODY");
    write_file(staging / "Meshes/Armor/Armor.nif", "ARMOR");

    const std::string xml = R"(<config>
  <moduleName>WinPaths</moduleName>
  <requiredInstallFiles>
    <file source="Skeleton Rig\HDT\body.nif" priority="0"/>
    <file source="meshes\armor\armor.nif" priority="0"/>
    <folder source="Skeleton Rig\HDT" destination="Data\SkeletonRig" priority="0"/>
  </requiredInstallFiles>
  <installSteps/>
</config>)";

    auto vm = view_model(xml);
    FomodFileInstaller installer(staging, vm);
    std::vector<std::string> missing;
    const bool applied = installer.apply(&missing);
    REQUIRE(applied);

    // Nothing reported missing: backslashes resolved as separators and every
    // component matched case-insensitively against the on-disk tree.
    REQUIRE(missing.empty());

    // Backslash source resolved to Skeleton Rig/HDT/body.nif (source-relative
    // destination keeps the source's relative path).
    REQUIRE(fs::exists(staging / "Skeleton Rig/HDT/body.nif"));

    // Case-insensitive match: meshes\armor\armor.nif resolved to the on-disk
    // Meshes/Armor/Armor.nif; the install destination uses the FOMOD's stated
    // path verbatim (backslash normalized), like FOMOD Plus.
    REQUIRE(fs::exists(staging / "Meshes/Armor/Armor.nif") == false);
    REQUIRE(fs::exists(staging / "meshes/armor/armor.nif"));

    // Folder children copied under the normalized backslash destination.
    REQUIRE(fs::exists(staging / "Data/SkeletonRig/body.nif"));
    std::printf("PASS: fomod_installer — Windows backslash paths + case-insensitive resolution\n");
}

}  // namespace

// Probe for a case-sensitive filesystem. macOS APFS is case-insensitive by
// default, so tests that assert case-variant dirs/files coexist cannot pass.
static bool is_case_sensitive_fs() {
    const fs::path base = fs::temp_directory_path() / "gmm_case_probe";
    std::error_code ec;
    fs::create_directories(base / "A", ec);
    const bool result = !fs::exists(base / "a", ec); // CI fs -> "a" exists
    fs::remove_all(base, ec);
    return result;
}

TEST_CASE("fomod installer", "[engine]") {
    if (!is_case_sensitive_fs()) {
        WARN("Skipping: filesystem is case-insensitive (macOS APFS default)");
        return;
    }

    test_priority_remap_and_folders();
    test_folder_empty_destination();
    test_path_traversal_guard();
    test_windows_paths_case_insensitive();
    test_generate_fomod_json();
}
