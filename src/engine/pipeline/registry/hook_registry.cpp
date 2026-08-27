#include "engine/pipeline/registry/hook_registry.h"
#include "engine/core/log/logger.h"

#include <algorithm>

namespace engine {

void HookRegistry::register_hook(const std::string& tag,
                                 HookFn handler,
                                 int priority,
                                 const std::string& plugin_id) {
    HookEntry entry;
    entry.tag = tag;
    entry.handler = std::move(handler);
    entry.priority = priority;
    entry.plugin_id = plugin_id;

    hooks_.push_back(std::move(entry));
}

void HookRegistry::fire(const std::string& tag, Mod& mod, PipelineContext& ctx) const {
    // Collect hooks for this tag
    std::vector<const HookEntry*> matching;
    for (const auto& hook : hooks_) {
        if (hook.tag == tag) {
            matching.push_back(&hook);
        }
    }

    // Sort by priority (highest first)
    std::sort(matching.begin(), matching.end(),
        [](const HookEntry* a, const HookEntry* b) {
            return a->priority > b->priority;
        });

    // Fire each hook
    for (const auto* hook : matching) {
        hook->handler(mod, ctx);
    }
}

std::vector<HookEntry> HookRegistry::hooks_for(const std::string& tag) const {
    std::vector<HookEntry> result;
    for (const auto& hook : hooks_) {
        if (hook.tag == tag) {
            result.push_back(hook);
        }
    }
    return result;
}

bool HookRegistry::has_hooks(const std::string& tag) const {
    for (const auto& hook : hooks_) {
        if (hook.tag == tag) return true;
    }
    return false;
}

void HookRegistry::clear_plugin_hooks(const std::string& plugin_id) {
    hooks_.erase(
        std::remove_if(hooks_.begin(), hooks_.end(),
            [&plugin_id](const HookEntry& entry) {
                return entry.plugin_id == plugin_id;
            }),
        hooks_.end());
}

void HookRegistry::clear() {
    hooks_.clear();
}

}  // namespace engine
