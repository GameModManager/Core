#pragma once

#include "engine/model/mod.h"

namespace engine {

struct PipelineContext;

class Stage {
public:
    virtual ~Stage() = default;
    virtual bool execute(Mod& mod, PipelineContext& ctx) = 0;
};

}  // namespace engine
