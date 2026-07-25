#include "engine/pipeline/pipeline.h"
#include "engine/pipeline/fetch_stage.h"
#include "engine/pipeline/extract_stage.h"
#include "engine/pipeline/install_stage.h"
#include "engine/pipeline/stage_stage.h"
#include "engine/pipeline/resolve_stage.h"
#include "engine/pipeline/deploy_stage.h"
#include "engine/pipeline/launch_stage.h"
#include "engine/pipeline/sync_stage.h"
#include "engine/model/mod.h"

#include <cassert>
#include <cstdio>

int main() {
    using namespace engine;

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
    return 0;
}
