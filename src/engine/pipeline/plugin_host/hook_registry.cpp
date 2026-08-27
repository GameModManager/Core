#include "engine/pipeline/plugin_host/hook_registry.h"

#include "engine/core/log/logger.h"

#include <algorithm>

HookRegistry& HookRegistry::instance() {
    static HookRegistry inst;
    return inst;
}

void HookRegistry::register_hook(const char* tag, GmmHookFnV2 fn, int priority,
                                 void* user_data, const char* plugin_path) {
    if (!fn) return;
    HookRegistration r;
    r.tag = tag ? tag : "";
    r.fn = fn;
    r.priority = priority;
    r.user_data = user_data;
    r.plugin_path = plugin_path ? plugin_path : "";
    hooks_.push_back(std::move(r));
}

void HookRegistry::dispatch(const char* tag, void* data) {
    if (!tag) return;

    // Collect hooks for this tag, then fire in priority order (highest first).
    std::vector<const HookRegistration*> matching;
    for (const auto& h : hooks_) {
        if (h.tag == tag) matching.push_back(&h);
    }
    std::sort(matching.begin(), matching.end(),
        [](const HookRegistration* a, const HookRegistration* b) {
            return a->priority > b->priority;
        });

    for (const auto* h : matching) {
        if (h->fn) h->fn(tag, data, h->user_data);
    }
}

void HookRegistry::clear_plugin(const char* plugin_path) {
    if (!plugin_path) return;
    std::string p = plugin_path;
    hooks_.erase(
        std::remove_if(hooks_.begin(), hooks_.end(),
            [&p](const HookRegistration& h) { return h.plugin_path == p; }),
        hooks_.end());
}
