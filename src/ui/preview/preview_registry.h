#pragma once

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "gmm_abi_v2.h" // GmmPreviewFn
#include "engine/core/log/logger.h"

// ---------------------------------------------------------------------------
// PreviewRegistry — v2 IPluginPreview backing store.
//
// This is a header-only singleton on purpose: the engine's plugin loader
// (gmm_engine, which is Qt-free and must NOT link the UI library) populates it
// from the v2 register_preview callback, while the UI's PreviewWindow queries
// it. Keeping the implementation inline (and the stored types opaque: function
// pointers + void*) means neither side needs a link or Qt dependency on the
// other — the engine only ever sees void* and GmmPreviewFn, and the UI casts
// the returned void* to QWidget*.
//
// A plugin registers a generator for a file extension (e.g. ".dds", ".nif").
// When the preview window opens a file it asks the registry for that
// extension; if a plugin claimed it, the generator is invoked and the returned
// QWidget* is embedded in the preview panel. Otherwise the window falls back to
// its built-in image/text/animation previews.
// ---------------------------------------------------------------------------

namespace ui::preview {

struct PreviewRegistryEntry {
  std::string extension;     // normalized: lowercase, leading dot (e.g. ".dds")
  GmmPreviewFn fn = nullptr; // plugin generator: (file_path, preview_data,
                             // user_data) -> QWidget*
  void *preview_data = nullptr;
  void *user_data = nullptr;
  std::string
      plugin_path; // owning plugin path; used to drop the entry on unload
};

class PreviewRegistry {
public:
  static PreviewRegistry &instance() {
    static PreviewRegistry reg;
    return reg;
  }

  // Register a plugin preview generator for a file extension. The extension is
  // normalized internally (lowercased, leading dot ensured). plugin_path is the
  // owning plugin's load path, used to drop the entry when the plugin unloads
  // so the stored function pointer never dangles after dlclose.
  void register_preview(const std::string &extension, GmmPreviewFn fn,
                        void *preview_data, void *user_data,
                        const std::string &plugin_path) {
    if (!fn)
      return;
    const std::string norm = normalize_extension(extension);
    engine::Logger::instance().debug("[PreviewRegistry] register_preview: ext=" + norm +
                             " fn=" + std::to_string(reinterpret_cast<uintptr_t>(fn)) +
                             " plugin=" + plugin_path);
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.push_back(PreviewRegistryEntry{norm, fn, preview_data, user_data,
                                            plugin_path});
    engine::Logger::instance().debug("[PreviewRegistry] total entries after register: " +
                             std::to_string(entries_.size()));
  }

  // Drop every entry owned by a plugin (called from PluginLoader::unload_all
  // before dlclose so no dangling function pointer survives).
  void clear_plugin(const std::string &plugin_path) {
    if (plugin_path.empty())
      return;
    std::lock_guard<std::mutex> lock(mutex_);
    std::erase_if(entries_, [&](const PreviewRegistryEntry &e) {
      return e.plugin_path == plugin_path;
    });
  }

  // Whether any plugin registered a preview for this file's extension.
  [[nodiscard]] bool has_preview(const std::string &file_path) const {
    const std::string ext = normalize_extension(
        std::filesystem::path(file_path).extension().string());
    engine::Logger::instance().debug("[PreviewRegistry] has_preview: file=" + file_path +
                             " ext=" + ext + " entries_count=" + std::to_string(entries_.size()));
    if (ext.empty()) {
      engine::Logger::instance().debug("[PreviewRegistry] has_preview: ext empty, returning false");
      return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    bool found = std::any_of(entries_.begin(), entries_.end(),
                             [&](const PreviewRegistryEntry &e) {
                               engine::Logger::instance().debug("[PreviewRegistry] has_preview: checking entry ext=" + e.extension);
                               return e.extension == ext;
                             });
    engine::Logger::instance().debug("[PreviewRegistry] has_preview: result=" + std::string(found ? "TRUE" : "FALSE"));
    return found;
  }

  // Invoke the registered generator for file_path's extension. Returns the
  // plugin-provided QWidget* as a void* (the caller casts to QWidget*), or
  // nullptr if no plugin handles the extension or the generator declined.
  [[nodiscard]] void *create_preview(const std::string &file_path) const {
    const std::string ext = normalize_extension(
        std::filesystem::path(file_path).extension().string());
    engine::Logger::instance().debug("[PreviewRegistry] create_preview: file=" + file_path +
                             " ext=" + ext + " entries_count=" + std::to_string(entries_.size()));
    if (ext.empty()) {
      engine::Logger::instance().debug("[PreviewRegistry] create_preview: ext empty, returning nullptr");
      return nullptr;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < entries_.size(); ++i) {
      const auto &e = entries_[i];
      engine::Logger::instance().debug("[PreviewRegistry] create_preview: entry[" + std::to_string(i) +
                               "] ext=" + e.extension + " fn=" +
                               std::to_string(reinterpret_cast<uintptr_t>(e.fn)));
    }
    auto it = std::find_if(entries_.begin(), entries_.end(),
                           [&](const PreviewRegistryEntry &e) {
                             return e.extension == ext && e.fn != nullptr;
                           });
    if (it == entries_.end()) {
      engine::Logger::instance().debug("[PreviewRegistry] create_preview: no match found, returning nullptr");
      return nullptr;
    }
    engine::Logger::instance().debug("[PreviewRegistry] create_preview: match found, calling fn");
    return it->fn(file_path.c_str(), it->preview_data, it->user_data);
  }

private:
  PreviewRegistry() = default;
  PreviewRegistry(const PreviewRegistry &) = delete;
  PreviewRegistry &operator=(const PreviewRegistry &) = delete;

  static std::string normalize_extension(std::string ext) {
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    if (ext.empty() || ext[0] != '.')
      ext = "." + ext;
    return ext;
  }

  mutable std::mutex mutex_;
  std::vector<PreviewRegistryEntry> entries_;
};

} // namespace ui::preview
