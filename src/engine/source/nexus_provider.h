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
    // Returns the real Nexus file metadata (files/{file}.json): the archive
    // name (file_name) for correct naming/extension and the display name.
    SourceDownloadInfo resolve_download_info(const Mod& mod) const override;
};

} // namespace engine
