#pragma once

// v2 IPluginTool registry - tracks plugin-provided tool callbacks keyed by
// tool_id, with the owning plugin path so the loader can drop a plugin's tools
// on unload (before dlclose) and never invoke a dangling function pointer.
//
// This is distinct from platform/tools/external_tool.h's ToolRegistry, which
// stores ExternalTool descriptors (executable detection, args, game scoping)
// used to build the Tools menu. The v2 IPluginTool registry stores the raw
// GmmToolInvokeFn + user_data the plugin handed us, exactly as the ABI
// promised.

#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace engine {

// Raw v2 tool invoke callback. Identical layout to GmmToolInvokeFn in
// gmm_abi_v2.h; declared locally so this header stays free of the ABI header.
using ToolInvokeFn = void (*)(void *user_data);

struct ToolEntry {
  std::string tool_id;
  std::string kind; // "tool", "workshop", "advisory", ...
  ToolInvokeFn fn = nullptr;
  void *user_data = nullptr;
  std::string plugin_path; // owning plugin (for clear_plugin on unload)
};

class PluginToolRegistry {
public:
  static PluginToolRegistry &instance();

  // Register (or replace) a plugin-provided tool callback.
  void register_tool(const std::string &tool_id, const std::string &kind,
                     ToolInvokeFn fn, void *user_data,
                     const std::string &plugin_path);

  // Invoke a registered tool by id. Returns false if unknown or fn is null.
  bool invoke(const std::string &tool_id) const;

  // Snapshot of a single tool, or nullopt if unknown.
  [[nodiscard]] std::optional<ToolEntry> find(const std::string &tool_id) const;

  // All registered tools (caller-owned copy).
  [[nodiscard]] std::vector<ToolEntry> all() const;

  [[nodiscard]] bool contains(const std::string &tool_id) const;

  // Drop every tool owned by plugin_path (called on plugin unload).
  void clear_plugin(const std::string &plugin_path);

  void clear();

private:
  PluginToolRegistry() = default;

  mutable std::mutex mutex_;
  std::vector<ToolEntry> tools_;
};

} // namespace engine
