#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace engine {

struct Mod;
struct PipelineContext;

} // namespace engine

namespace engine::Source {

// Info a provider can resolve for a mod before download.
struct SourceDownloadInfo {
    std::string archive_name;   // real archive filename incl. extension (empty
                                // = keep the default "<source_id>[-<file_id>].zip")
    std::string display_name;   // human-readable mod/file name for the UI
};

class Interface {
public:
    virtual ~Interface() = default;
    virtual std::string source_type() const = 0;
    // Download a mod to dest_path. Returns true on success. ctx is mutable so
    // providers can signal a pause (PipelineContext::download_paused) and read
    // the resume offset / abort flag.
    virtual bool fetch(const ::engine::Mod& mod, ::engine::PipelineContext& ctx,
                       const std::filesystem::path& dest_path) = 0;
    // Optional: resolve metadata about a mod before download (real archive
    // name, display name). Called by FetchStage on the worker thread before
    // fetch(). An empty struct means "no info available".
    virtual SourceDownloadInfo resolve_download_info(const ::engine::Mod& mod) const {
        return {};
    }

    // Human-readable provider name for the UI (Sources tab).
    virtual std::string display_name() const { return source_type(); }
};

} // namespace engine::Source
