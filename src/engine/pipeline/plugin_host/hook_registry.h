#pragma once
#include <string>
#include <vector>
#include <functional>

// v2 ABI hook callback signature (gmm_abi_v2.h) - pure C, Qt-free.
#include "gmm_abi_v2.h"

// v2 behavior-injection hook registry. Plugins register a GmmHookFnV2 under a
// tag (e.g. "before_deploy", "after_scan", "conflict_resolution"); the pipeline
// dispatches a tag with an opaque data pointer and every registered hook for
// that tag runs in priority order. This is the v2 counterpart of the
// instance-based engine::HookRegistry used by the v1 ABI path; it stores the
// raw ABI function pointer + user_data so the engine can fire it directly
// without wrapping in a std::function.
struct HookRegistration {
    std::string tag;
    GmmHookFnV2 fn;  // from gmm_abi_v2.h
    int priority;
    void* user_data;
    std::string plugin_path;
};

class HookRegistry {
public:
    static HookRegistry& instance();
    void register_hook(const char* tag, GmmHookFnV2 fn, int priority, void* user_data, const char* plugin_path);
    void dispatch(const char* tag, void* data);
    void clear_plugin(const char* plugin_path);
private:
    std::vector<HookRegistration> hooks_;
};
