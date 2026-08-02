#include "engine/pipeline/pipeline.h"
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
    // a FOMOD archive aborts the pipeline with a warning (not implemented).
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
    {
        TempDir fomod;
        std::filesystem::create_directories(fomod.root / "fomod");

        FomodStage fomod_stage;
        PipelineContext ctx;
        Mod mod;
        mod.id = "fomod-mod";
        mod.name = "Fomod Mod";
        mod.state = ModState::Extracted;

        ModFile staging;
        staging.relative_path = fomod.root.string();
        mod.files.push_back(staging);

        std::ofstream cfg(fomod.root / "fomod" / "ModuleConfig.xml");
        cfg << "<config><moduleName>Test</moduleName></config>";
        cfg.close();

        assert(!fomod_stage.execute(mod, ctx));
        std::printf("PASS: pipeline_test — FOMOD archive aborts at FomodStage\n");
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
