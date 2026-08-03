#include "engine/pipeline/pipeline.h"
#include "engine/fomod/fomod_view_model.h"
#include "engine/pipeline/fetch_stage.h"
#include "engine/pipeline/extract_stage.h"
#include "engine/pipeline/fomod_stage.h"
#include "engine/pipeline/install_stage.h"
#include "engine/pipeline/stage_stage.h"
#include "engine/pipeline/resolve_stage.h"
#include "engine/pipeline/deploy_stage.h"
#include "engine/pipeline/launch_stage.h"
#include "engine/pipeline/sync_stage.h"
#include "engine/model/mod.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace {
using namespace engine;

// Build a temp directory tree rooted at a unique path under the system tmp.
// PID + counter keeps paths unique even when an earlier aborted run leaked its
// directory (otherwise a leaked "My Mod 2" from a killed run would contaminate
// the next run's rename-collision expectations).
struct TempDir {
    std::filesystem::path root;
    TempDir() {
        root = std::filesystem::temp_directory_path() /
               ("gmm_pipeline_test_" + std::to_string(::getpid()) + "_" +
                std::to_string(counter_++));
        std::filesystem::create_directories(root);
    }
    ~TempDir() { std::error_code ec; std::filesystem::remove_all(root, ec); }
    static int counter_;
};
int TempDir::counter_ = 0;

// A staging dir with a real FOMOD: fomod/ModuleConfig.xml plus a required
// Core.esm and two SelectAny plugin files (Patches/HighRes.esp, Patches/Lite.esp).
struct FomodFixture {
    TempDir tmp;
    std::filesystem::path staging;
    explicit FomodFixture(const std::string& config, const std::string& fomodDir = "fomod") {
        staging = tmp.root / "staging";
        std::filesystem::create_directories(staging / fomodDir);
        std::filesystem::create_directories(staging / "Patches");
        std::ofstream(staging / "Core.esm") << "core";
        std::ofstream(staging / "Patches" / "HighRes.esp") << "hr";
        std::ofstream(staging / "Patches" / "Lite.esp") << "lite";
        std::ofstream(staging / fomodDir / "ModuleConfig.xml") << config;
    }
    Mod make_mod(const std::string& name = "Fomod Mod") {
        Mod mod;
        mod.id = "fomod-mod";
        mod.name = name;
        mod.state = ModState::Extracted;
        ModFile f;
        f.relative_path = staging.string();
        mod.files.push_back(f);
        return mod;
    }
};

const char* kBasicConfig = R"(<config>
  <moduleName>Test FOMOD</moduleName>
  <requiredInstallFiles>
    <file source="Core.esm" destination=""/>
  </requiredInstallFiles>
  <installSteps>
    <installStep name="Core">
      <optionalFileGroups>
        <group name="Options" type="SelectAny">
          <plugins order="Ascending">
            <plugin name="High Res">
              <files><file source="Patches/HighRes.esp"/></files>
            </plugin>
            <plugin name="Lite">
              <files><file source="Patches/Lite.esp"/></files>
            </plugin>
          </plugins>
        </group>
      </optionalFileGroups>
    </installStep>
  </installSteps>
</config>)";

const char* kHighResChoices = R"({"steps":[{"name":"Core","groups":[{"name":"Options","plugins":["High Res"],"deselected":[]}]}]})";

// A helper that returns a FomodDecision accepting with High Res selected.
auto accept_high_res =
    [](const std::shared_ptr<FomodViewModel>&, const std::filesystem::path&, const std::string&,
       const std::string&) {
        FomodDecision d;
        d.accept = true;
        d.choices_json = kHighResChoices;
        return d;
    };
}  // namespace

int main() {
    {
        Pipeline pipeline;

        PipelineContext ctx;
        pipeline.set_context(ctx);

        pipeline.add_stage(std::make_unique<FetchStage>());
        pipeline.add_stage(std::make_unique<ExtractStage>());
        pipeline.add_stage(std::make_unique<InstallStage>());
        pipeline.add_stage(std::make_unique<StageStage>());
        pipeline.add_stage(std::make_unique<ResolveStage>());
        pipeline.add_stage(std::make_unique<DeployStage>());
        pipeline.add_stage(std::make_unique<LaunchStage>());
        pipeline.add_stage(std::make_unique<SyncStage>());

        Mod mod;
        mod.id = "test-mod-001";
        mod.name = "Test Mod";
        mod.version = "1.0";

        assert(mod.state == ModState::Downloaded);
        assert(pipeline.run(mod));
        assert(mod.state == ModState::Deployed);
        assert(mod.id == "test-mod-001");

        std::printf("PASS: pipeline_test — mod flowed through all 8 stages\n");
    }

    // FomodStage: a plain archive (no fomod/ModuleConfig.xml) passes through,
    // and a FOMOD archive is installed via the fomod_query_cb wizard.
    {
        TempDir plain;
        std::filesystem::create_directories(plain.root / "textures");

        FomodStage fomod;
        PipelineContext ctx;
        Mod mod;
        mod.id = "plain-mod";
        mod.name = "Plain Mod";
        mod.state = ModState::Extracted;

        ModFile staging;
        staging.relative_path = plain.root.string();
        mod.files.push_back(staging);

        assert(fomod.execute(mod, ctx));
        std::printf("PASS: pipeline_test — non-FOMOD archive passes FomodStage\n");
    }

    // (a) wizard accepts → FOMOD files installed, fomod/ pruned, choices passed on
    {
        FomodFixture fix(kBasicConfig);
        FomodStage fomod;
        PipelineContext ctx;
        Mod mod = fix.make_mod();
        ctx.fomod_query_cb = [&](const std::shared_ptr<FomodViewModel>&, const std::filesystem::path& root,
                                 const std::string& suggested, const std::string& previous) {
            assert(root == fix.staging);
            assert(suggested == "Fomod Mod");
            assert(previous.empty());
            FomodDecision d;
            d.accept = true;
            d.choices_json = kHighResChoices;
            d.mod_name = "Installed Name";
            return d;
        };
        assert(fomod.execute(mod, ctx));
        assert(mod.name == "Installed Name");
        assert(ctx.fomod_choices_json == kHighResChoices);
        assert(std::filesystem::exists(fix.staging / "Core.esm"));
        assert(std::filesystem::exists(fix.staging / "Patches" / "HighRes.esp"));
        assert(!std::filesystem::exists(fix.staging / "Patches" / "Lite.esp"));
        assert(!std::filesystem::exists(fix.staging / "fomod"));
        std::printf("PASS: pipeline_test — FOMOD wizard accept installs selected files\n");
    }

    // (b) wizard cancel aborts the pipeline
    {
        FomodFixture fix(kBasicConfig);
        FomodStage fomod;
        PipelineContext ctx;
        Mod mod = fix.make_mod();
        ctx.fomod_query_cb = [](const std::shared_ptr<FomodViewModel>&, const std::filesystem::path&,
                                const std::string&, const std::string&) {
            return FomodDecision{};  // accept == false
        };
        assert(!fomod.execute(mod, ctx));
        assert(ctx.fomod_choices_json.empty());
        std::printf("PASS: pipeline_test — FOMOD wizard cancel aborts\n");
    }

    // (b3) capitalized "Fomod/" layout (XPMSE-style) is still detected — Windows
    // mod authors ship any casing of the installer dir; detection is
    // case-insensitive like FOMOD Plus's scanner.
    {
        FomodFixture fix(kBasicConfig, "Fomod");
        FomodStage fomod;
        PipelineContext ctx;
        Mod mod = fix.make_mod();
        int queried = 0;
        ctx.fomod_query_cb = [&](const std::shared_ptr<FomodViewModel>&, const std::filesystem::path& root,
                                 const std::string&, const std::string&) {
            assert(root == fix.staging);
            ++queried;
            FomodDecision d;
            d.accept = true;
            d.choices_json = kHighResChoices;
            return d;
        };
        assert(fomod.execute(mod, ctx));
        assert(queried == 1);
        assert(std::filesystem::exists(fix.staging / "Patches" / "HighRes.esp"));
        assert(!std::filesystem::exists(fix.staging / "Patches" / "Lite.esp"));
        assert(!std::filesystem::exists(fix.staging / "Fomod"));
        std::printf("PASS: pipeline_test — capitalized Fomod/ layout detected case-insensitively\n");
    }

    // (b2) wizard Manual → archive contents install as-is (fomod/ pruned)
    {
        FomodFixture fix(kBasicConfig);
        FomodStage fomod;
        PipelineContext ctx;
        Mod mod = fix.make_mod();
        ctx.fomod_query_cb = [](const std::shared_ptr<FomodViewModel>&, const std::filesystem::path&,
                                const std::string&, const std::string&) {
            FomodDecision d;
            d.manual = true;
            d.mod_name = "Manual Name";
            return d;
        };
        assert(fomod.execute(mod, ctx));
        assert(mod.name == "Manual Name");
        assert(ctx.fomod_choices_json.empty());
        assert(std::filesystem::exists(fix.staging / "Core.esm"));
        assert(std::filesystem::exists(fix.staging / "Patches" / "HighRes.esp"));
        assert(std::filesystem::exists(fix.staging / "Patches" / "Lite.esp"));
        assert(!std::filesystem::exists(fix.staging / "fomod"));
        std::printf("PASS: pipeline_test — FOMOD Manual install keeps raw contents\n");
    }

    // (c) <csharpScript> FOMODs are rejected with a clear warning
    {
        FomodFixture fix(
            "<config><moduleName>CS</moduleName><csharpScript>/* unsupported */</csharpScript></config>");
        FomodStage fomod;
        PipelineContext ctx;
        Mod mod = fix.make_mod();
        assert(!fomod.execute(mod, ctx));
        std::printf("PASS: pipeline_test — C# script FOMOD aborts\n");
    }

    // (d) headless (no callback) + no previous choices → abort, never guess
    {
        FomodFixture fix(kBasicConfig);
        FomodStage fomod;
        PipelineContext ctx;
        Mod mod = fix.make_mod();
        assert(!fomod.execute(mod, ctx));
        std::printf("PASS: pipeline_test — headless FOMOD without choices aborts\n");
    }

    // (e) headless with previously persisted choices → restored + installed
    {
        FomodFixture fix(kBasicConfig);
        auto mods = fix.tmp.root / "mods";
        std::filesystem::create_directories(mods / "Restored Mod");
        std::ofstream(mods / "Restored Mod" / "meta.ini")
            << "[fomod]\nchoices=" << kHighResChoices << "\n";
        FomodStage fomod;
        PipelineContext ctx;
        ctx.mods_dir = mods;
        Mod mod = fix.make_mod("Restored Mod");
        assert(fomod.execute(mod, ctx));
        assert(ctx.fomod_choices_json == kHighResChoices);
        assert(std::filesystem::exists(fix.staging / "Patches" / "HighRes.esp"));
        assert(!std::filesystem::exists(fix.staging / "Patches" / "Lite.esp"));
        std::printf("PASS: pipeline_test — headless FOMOD restores previous choices\n");
    }

    // (f) files missing from the archive abort by default, proceed on request
    {
        const char* kMissingConfig = R"(<config>
  <moduleName>Broken</moduleName>
  <requiredInstallFiles>
    <file source="Core.esm" destination=""/>
    <file source="Missing.txt" destination=""/>
  </requiredInstallFiles>
</config>)";
        {
            FomodFixture fix(kMissingConfig);
            FomodStage fomod;
            PipelineContext ctx;
            Mod mod = fix.make_mod();
            ctx.fomod_query_cb = [](const std::shared_ptr<FomodViewModel>&, const std::filesystem::path&,
                                    const std::string&, const std::string&) {
                FomodDecision d;
                d.accept = true;
                return d;
            };
            assert(!fomod.execute(mod, ctx));
            std::printf("PASS: pipeline_test — FOMOD missing files abort by default\n");
        }
        {
            FomodFixture fix(kMissingConfig);
            FomodStage fomod;
            PipelineContext ctx;
            Mod mod = fix.make_mod();
            ctx.fomod_query_cb = [](const std::shared_ptr<FomodViewModel>&, const std::filesystem::path&,
                                    const std::string&, const std::string&) {
                FomodDecision d;
                d.accept = true;
                d.ignore_missing = true;
                return d;
            };
            assert(fomod.execute(mod, ctx));
            assert(std::filesystem::exists(fix.staging / "Core.esm"));
            assert(!std::filesystem::exists(fix.staging / "fomod"));
            std::printf("PASS: pipeline_test — FOMOD missing files ignored on request\n");
        }
    }

    // (g) full flow: FomodStage + InstallStage persist [fomod] choices in the
    // mod folder's meta.ini, and the installed folder contains only the
    // selected files.
    {
        FomodFixture fix(kBasicConfig);
        auto mods = fix.tmp.root / "mods";
        std::filesystem::create_directories(mods);
        FomodStage fomod;
        PipelineContext ctx;
        ctx.mods_dir = mods;
        Mod mod = fix.make_mod();
        ctx.fomod_query_cb = accept_high_res;
        assert(fomod.execute(mod, ctx));
        InstallStage install;
        assert(install.execute(mod, ctx));
        auto meta_path = mods / "Fomod Mod" / "meta.ini";
        assert(std::filesystem::exists(meta_path));
        std::ifstream f(meta_path);
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        assert(content.find("[fomod]") != std::string::npos);
        assert(content.find(kHighResChoices) != std::string::npos);
        assert(std::filesystem::exists(mods / "Fomod Mod" / "Core.esm"));
        assert(std::filesystem::exists(mods / "Fomod Mod" / "Patches" / "HighRes.esp"));
        assert(!std::filesystem::exists(mods / "Fomod Mod" / "Patches" / "Lite.esp"));
        std::printf("PASS: pipeline_test — FOMOD choices persisted in mod folder meta.ini\n");
    }

    // InstallStage overwrite query flow: when the target mod folder already
    // exists, overwrite_query_cb decides Merge/Replace/Rename/Cancel instead
    // of the silent-replace default.
    {
        TempDir env;
        auto staging = env.root / "staging";
        auto mods = env.root / "mods";
        std::filesystem::create_directories(mods);

        // InstallStage deletes the staging dir after each install, so every
        // test recreates it.
        auto reset_staging = [&] {
            std::error_code ec;
            std::filesystem::remove_all(staging, ec);
            std::filesystem::create_directories(staging);
            std::ofstream(staging / "b.txt") << "b";
        };

        auto make_mod = [&](const std::filesystem::path& src) {
            Mod mod;
            mod.id = "qmod";
            mod.name = "My Mod";
            mod.version = "1.0";
            mod.state = ModState::Extracted;
            ModFile f;
            f.relative_path = src.string();
            mod.files.push_back(f);
            return mod;
        };
        auto install = [&](Mod& mod, PipelineContext& ctx) {
            InstallStage stage;
            return stage.execute(mod, ctx);
        };

        // (a) no callback -> silent replace
        {
            reset_staging();
            auto dir = mods / "My Mod";
            std::filesystem::create_directories(dir);
            std::ofstream(dir / "old.txt") << "old";
            PipelineContext ctx;
            ctx.mods_dir = mods;
            Mod mod = make_mod(staging);
            assert(install(mod, ctx));
            assert(mod.state == ModState::Installed);
            assert(!std::filesystem::exists(dir / "old.txt"));
            assert(std::filesystem::exists(dir / "b.txt"));
            std::printf("PASS: install overwrite — headless silent replace\n");
        }

        // (b) Merge keeps existing files and adds the new ones
        {
            reset_staging();
            auto dir = mods / "My Mod";
            std::filesystem::create_directories(dir);
            std::ofstream(dir / "old.txt") << "old";
            PipelineContext ctx;
            ctx.mods_dir = mods;
            ctx.overwrite_query_cb = [](const std::string&) {
                return OverwriteDecision{OverwriteAction::Merge};
            };
            Mod mod = make_mod(staging);
            assert(install(mod, ctx));
            assert(mod.state == ModState::Installed);
            assert(std::filesystem::exists(dir / "old.txt"));
            assert(std::filesystem::exists(dir / "b.txt"));
            std::printf("PASS: install overwrite — merge keeps existing files\n");
        }

        // (c) Replace deletes the old folder before installing
        {
            reset_staging();
            auto dir = mods / "My Mod";
            std::filesystem::create_directories(dir);
            std::ofstream(dir / "old.txt") << "old";
            PipelineContext ctx;
            ctx.mods_dir = mods;
            ctx.overwrite_query_cb = [](const std::string&) {
                return OverwriteDecision{OverwriteAction::Replace};
            };
            Mod mod = make_mod(staging);
            assert(install(mod, ctx));
            assert(mod.state == ModState::Installed);
            assert(!std::filesystem::exists(dir / "old.txt"));
            assert(std::filesystem::exists(dir / "b.txt"));
            std::printf("PASS: install overwrite — replace deletes old files\n");
        }

        // (d) Replace + backup copies the old folder to <name>_backup first
        {
            reset_staging();
            auto dir = mods / "My Mod";
            std::filesystem::create_directories(dir);
            std::ofstream(dir / "old.txt") << "old";
            PipelineContext ctx;
            ctx.mods_dir = mods;
            ctx.overwrite_query_cb = [](const std::string&) {
                return OverwriteDecision{OverwriteAction::Replace, /*backup=*/true};
            };
            Mod mod = make_mod(staging);
            assert(install(mod, ctx));
            auto backup = mods / "My Mod_backup";
            assert(std::filesystem::exists(backup / "old.txt"));
            assert(!std::filesystem::exists(dir / "old.txt"));
            std::printf("PASS: install overwrite — replace keeps a backup\n");
        }

        // (e) Rename installs under the new folder and leaves the old one alone
        {
            reset_staging();
            auto dir = mods / "My Mod";
            std::filesystem::create_directories(dir);
            std::ofstream(dir / "old.txt") << "old";
            PipelineContext ctx;
            ctx.mods_dir = mods;
            ctx.overwrite_query_cb = [](const std::string&) {
                OverwriteDecision d;
                d.action = OverwriteAction::Rename;
                d.new_name = "My Mod 2";
                return d;
            };
            Mod mod = make_mod(staging);
            assert(install(mod, ctx));
            assert(mod.state == ModState::Installed);
            assert(mod.id == "My Mod 2");
            assert(std::filesystem::exists(dir / "old.txt"));
            assert(std::filesystem::exists(mods / "My Mod 2" / "b.txt"));
            std::printf("PASS: install overwrite — rename installs separately\n");
        }

        // (f) Rename re-checks: a new name that also exists re-asks (loop)
        {
            reset_staging();
            auto dir = mods / "My Mod";
            auto dir2 = mods / "My Mod 2";
            std::filesystem::create_directories(dir);
            std::ofstream(dir / "old.txt") << "old";
            std::filesystem::create_directories(dir2);
            std::ofstream(dir2 / "old.txt") << "old";
            int calls = 0;
            PipelineContext ctx;
            ctx.mods_dir = mods;
            ctx.overwrite_query_cb = [&](const std::string&) {
                ++calls;
                OverwriteDecision d;
                d.action = OverwriteAction::Rename;
                d.new_name = calls == 1 ? "My Mod 2" : "My Mod 3";
                return d;
            };
            Mod mod = make_mod(staging);
            assert(install(mod, ctx));
            assert(mod.state == ModState::Installed);
            assert(calls == 2);
            assert(mod.id == "My Mod 3");
            assert(std::filesystem::exists(mods / "My Mod 3" / "b.txt"));
            std::printf("PASS: install overwrite — rename loop re-checks collisions\n");
        }

        // (g) Cancel aborts the install, leaving the existing folder untouched
        {
            reset_staging();
            auto dir = mods / "My Mod";
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);  // drop leftovers from earlier tests
            std::filesystem::create_directories(dir);
            std::ofstream(dir / "old.txt") << "old";
            PipelineContext ctx;
            ctx.mods_dir = mods;
            ctx.overwrite_query_cb = [](const std::string&) {
                return OverwriteDecision{OverwriteAction::Cancel};
            };
            Mod mod = make_mod(staging);
            assert(!install(mod, ctx));
            assert(mod.state != ModState::Installed);
            assert(std::filesystem::exists(dir / "old.txt"));
            assert(!std::filesystem::exists(dir / "b.txt"));
            std::printf("PASS: install overwrite — cancel aborts cleanly\n");
        }
    }

    return 0;
}
