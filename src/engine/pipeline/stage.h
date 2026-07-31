#pragma once

#include <string>

#include "engine/model/mod.h"

namespace engine {

struct PipelineContext;

class Stage {
public:
    virtual ~Stage() = default;
    virtual bool execute(Mod& mod, PipelineContext& ctx) = 0;

    // Display name of this stage (e.g. "Fetch"). Empty = unnamed.
    virtual std::string name() const { return {}; }

    // One-line description of what this stage does, shown in the pipeline
    // window card. Empty = no description.
    virtual std::string description() const { return {}; }

    // Human-readable exit condition shown in the pipeline window -
    // what must be true to move past this stage.
    virtual std::string condition() const { return {}; }
};

}  // namespace engine
