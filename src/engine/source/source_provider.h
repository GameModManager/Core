#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace engine {

struct Mod;
struct PipelineContext;

class SourceProvider {
public:
    virtual ~SourceProvider() = default;
    virtual std::string source_type() const = 0;
    // Download a mod to dest_path. Returns true on success.
    virtual bool fetch(const Mod& mod, const PipelineContext& ctx,
                       const std::filesystem::path& dest_path) = 0;
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
