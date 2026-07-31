#include "engine/pipeline/plugin_claim_stage.h"

#include <filesystem>

namespace engine {

PluginClaimStage::PluginClaimStage(std::string stage_name,
                                   std::string origin,
                                   StageFn handler)
    : stage_name_(std::move(stage_name))
    , origin_(std::move(origin))
    , handler_(std::move(handler)) {
}

bool PluginClaimStage::execute(Mod& mod, PipelineContext& ctx) {
    return handler_ ? handler_(mod, ctx) : false;
}

std::string PluginClaimStage::name() const {
    return stage_name_;
}

std::string PluginClaimStage::description() const {
    std::filesystem::path p(origin_);
    auto base = p.filename().string();
    if (base.empty() || base == origin_) return "Provided by plugin " + origin_;
    return "Provided by plugin " + base;
}

std::string PluginClaimStage::condition() const {
    // Show the providing plugin (basename) as the "exit condition" hint.
    std::filesystem::path p(origin_);
    auto base = p.filename().string();
    if (base.empty() || base == origin_) return "Provided by " + origin_;
    return "Provided by " + base;
}

}  // namespace engine
