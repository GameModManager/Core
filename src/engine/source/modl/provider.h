#pragma once

#include "engine/source/interface.h"

#include <string>

namespace engine::Source::Modl {

// Transport helper for the modl:// protocol. modl:// is a transport, not a
// source - this class exists so FetchStage can dispatch a direct-URL
// download when a mod carries `download_source_type == "direct"`. The actual
// source attribution (mod.pub vs. arbitrary host vs. manual) is derived from
// the direct URL's host by Router::derive_source_from_direct_url and stamped
// on the mod before the pipeline runs.
//
// Kept in the engine::Source::Modl namespace so the curl mechanics stay
// alongside the parse_modl code path; the registered source_type() is
// "direct" so the SourceRegistry lookup in FetchStage succeeds for any
// direct-URL download regardless of origin.
class Provider : public Interface {
public:
    std::string source_type() const override { return "direct"; }
    bool fetch(const ::engine::Mod& mod, ::engine::PipelineContext& ctx,
               const std::filesystem::path& dest_path) override;
    SourceDownloadInfo resolve_download_info(const ::engine::Mod& mod) const override;
    std::string display_name() const override;
};

} // namespace engine::Source::Modl
