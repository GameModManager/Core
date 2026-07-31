#pragma once

#include "engine/source/source_provider.h"

#include <string>

namespace engine {

struct NxmLink;

class NexusProvider : public SourceProvider {
public:
    std::string source_type() const override { return "nexus"; }
    bool fetch(const Mod& mod, PipelineContext& ctx,
               const std::filesystem::path& dest_path) override;
};

} // namespace engine
