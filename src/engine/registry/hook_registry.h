#pragma once

#include <functional>
#include <string>
#include <vector>

namespace engine {

struct Mod;
struct PipelineContext;

// Hook function signature
using HookFn = std::function<void(Mod&, PipelineContext&)>;

struct HookEntry {
    std::string tag;
    HookFn handler;
    int priority = 0;
    std::string plugin_id;
};

class HookRegistry {
public:
    void register_hook(const std::string& tag,
                       HookFn handler,
                       int priority = 0,
                       const std::string& plugin_id = "");

    // Fire all hooks for a tag, in priority order
    void fire(const std::string& tag, Mod& mod, PipelineContext& ctx) const;

    // Get all hooks for a tag (for debugging)
    [[nodiscard]] std::vector<HookEntry> hooks_for(const std::string& tag) const;

    // Check if any hooks exist for a tag
    [[nodiscard]] bool has_hooks(const std::string& tag) const;

    void clear();

private:
    std::vector<HookEntry> hooks_;
};

}  // namespace engine
