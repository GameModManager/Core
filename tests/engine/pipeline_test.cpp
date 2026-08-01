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

namespace {
using namespace engine;

// Build a temp directory tree rooted at a unique path under the system tmp.
struct TempDir {
    std::filesystem::path root;
    TempDir() {
        root = std::filesystem::temp_directory_path() /
               ("gmm_pipeline_test_" + std::to_string(counter_++));
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

    return 0;
}
