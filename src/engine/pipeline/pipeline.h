#pragma once

#include "engine/pipeline/stage.h"

#include <memory>
#include <vector>

namespace engine {

class Instance;
class ConflictIndex;
class Profile;
class DeploymentStrategy;
class OrderEncodingHook;

struct PipelineContext {
    Instance* instance = nullptr;
    ConflictIndex* conflict_index = nullptr;
    Profile* profile = nullptr;
    DeploymentStrategy* deploy_strategy = nullptr;
    OrderEncodingHook* order_hook = nullptr;
};

class Pipeline {
public:
    void set_context(PipelineContext ctx);
    void add_stage(std::unique_ptr<Stage> stage);
    bool run(Mod& mod);

private:
    PipelineContext ctx_;
    std::vector<std::unique_ptr<Stage>> stages_;
};

}  // namespace engine
