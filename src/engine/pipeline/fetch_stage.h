#pragma once

#include "engine/pipeline/stage.h"

namespace engine {

class FetchStage : public Stage {
public:
    bool execute(Mod& mod, PipelineContext& ctx) override;
};

}  // namespace engine
