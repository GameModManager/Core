#pragma once

#include "engine/source/interface.h"
#include "engine/source/workshop/workshop_client.h"

#include <memory>
#include <string>

namespace engine::Source::Steam {

class Provider : public Interface {
public:
    explicit Provider(const std::string& db_path,
                      int rate_limit = 60, int rate_window = 3600);

    std::string source_type() const override { return "steam"; }
    bool fetch(const ::engine::Mod& mod, ::engine::PipelineContext& ctx,
               const std::filesystem::path& dest_path) override;
    std::string display_name() const override;

    // Push updated rate-limit values to the live client (no-op if client
    // hasn't been created yet; the values are remembered for it).
    void set_rate_limit(int limit, int window);
    int rate_limit() const { return rate_limit_; }
    int rate_window() const { return rate_window_; }

private:
    std::unique_ptr<WorkshopClient> client_;
    std::string db_path_;
    int rate_limit_ = 60;
    int rate_window_ = 3600;
};

} // namespace engine::Source::Steam
