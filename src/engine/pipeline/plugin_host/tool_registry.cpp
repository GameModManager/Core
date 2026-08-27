#include "engine/pipeline/plugin_host/tool_registry.h"
#include "engine/core/log/logger.h"

#include <algorithm>

namespace engine {

PluginToolRegistry &PluginToolRegistry::instance() {
  static PluginToolRegistry inst;
  return inst;
}

void PluginToolRegistry::register_tool(const std::string &tool_id,
                                       const std::string &kind, ToolInvokeFn fn,
                                       void *user_data,
                                       const std::string &plugin_path) {
  if (tool_id.empty())
    return;
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto &t : tools_) {
    if (t.tool_id == tool_id) {
      t.kind = kind;
      t.fn = fn;
      t.user_data = user_data;
      t.plugin_path = plugin_path;
      return;
    }
  }
  tools_.push_back({tool_id, kind, fn, user_data, plugin_path});
  Logger::instance().debug("ToolRegistry: registered v2 tool '" + tool_id +
                           "' from " + plugin_path);
}

bool PluginToolRegistry::invoke(const std::string &tool_id) const {
  ToolInvokeFn fn = nullptr;
  void *ud = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &t : tools_) {
      if (t.tool_id == tool_id) {
        fn = t.fn;
        ud = t.user_data;
        break;
      }
    }
  }
  if (!fn)
    return false;
  fn(ud);
  return true;
}

std::optional<ToolEntry>
PluginToolRegistry::find(const std::string &tool_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto &t : tools_)
    if (t.tool_id == tool_id)
      return t;
  return std::nullopt;
}

std::vector<ToolEntry> PluginToolRegistry::all() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return tools_;
}

bool PluginToolRegistry::contains(const std::string &tool_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto &t : tools_)
    if (t.tool_id == tool_id)
      return true;
  return false;
}

void PluginToolRegistry::clear_plugin(const std::string &plugin_path) {
  if (plugin_path.empty())
    return;
  std::lock_guard<std::mutex> lock(mutex_);
  tools_.erase(std::remove_if(tools_.begin(), tools_.end(),
                              [&](const ToolEntry &t) {
                                return t.plugin_path == plugin_path;
                              }),
               tools_.end());
}

void PluginToolRegistry::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  tools_.clear();
}

} // namespace engine
