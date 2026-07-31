#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace engine {

struct Mod;
struct PipelineContext;

// Info a provider can resolve for a mod before download.
struct SourceDownloadInfo {
    std::string archive_name;   // real archive filename incl. extension (empty
                                // = keep the default "<source_id>[-<file_id>].zip")
    std::string display_name;   // human-readable mod/file name for the UI
};

class SourceProvider {
public:
    virtual ~SourceProvider() = default;
    virtual std::string source_type() const = 0;
    // Download a mod to dest_path. Returns true on success. ctx is mutable so
    // providers can signal a pause (PipelineContext::download_paused) and read
    // the resume offset / abort flag.
    virtual bool fetch(const Mod& mod, PipelineContext& ctx,
                       const std::filesystem::path& dest_path) = 0;
    // Optional: resolve metadata about a mod before download (real archive
    // name, display name). Called by FetchStage on the worker thread before
    // fetch(). An empty struct means "no info available".
    virtual SourceDownloadInfo resolve_download_info(const Mod& mod) const {
        return {};
    }
};

class SourceRegistry {
public:
    static SourceRegistry& instance();
    void register_provider(std::unique_ptr<SourceProvider> provider);
    SourceProvider* provider_for(const std::string& source_type) const;
    std::vector<std::string> available_sources() const;
private:
    SourceRegistry() = default;
    std::vector<std::unique_ptr<SourceProvider>> providers_;
};

} // namespace engine
