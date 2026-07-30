#pragma once

#include "engine/source/source_provider.h"
#include "engine/workshop/workshop_client.h"

#include <memory>
#include <string>

namespace engine {

class SteamWorkshopProvider : public SourceProvider {
public:
    explicit SteamWorkshopProvider(const std::string& db_path);

    std::string source_type() const override { return "steam"; }
    bool fetch(const Mod& mod, const PipelineContext& ctx,
               const std::filesystem::path& dest_path) override;

private:
    std::unique_ptr<WorkshopClient> client_;
    std::string db_path_;
};

} // namespace engine
