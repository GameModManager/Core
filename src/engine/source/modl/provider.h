#pragma once

#include "engine/source/interface.h"

#include <string>

namespace engine::Source::Modl {

// Downloads modl:// links - mod.pub + any site that emits a modl:// URL
// pointing at a direct https download. There is no API: the modlhandler
// already extracted the direct URL, so fetch() is a plain curl_download
// with no cookie/auth in v1. If a future site requires a session cookie,
// it can be added here alongside the LoversLab pattern.
class Provider : public Interface {
public:
    std::string source_type() const override { return "modl"; }
    bool fetch(const ::engine::Mod& mod, ::engine::PipelineContext& ctx,
               const std::filesystem::path& dest_path) override;
    SourceDownloadInfo resolve_download_info(const ::engine::Mod& mod) const override;
    std::string display_name() const override;
};

} // namespace engine::Source::Modl
