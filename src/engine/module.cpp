#include "engine/module.h"

#include "engine/log/logger.h"
#include "platform/platform.h"

#include <algorithm>

namespace engine {

// ---------------------------------------------------------------------------
// ModuleInfo::from_path
// ---------------------------------------------------------------------------
ModuleInfo ModuleInfo::from_path(const std::filesystem::path &path) {
  ModuleInfo info;
  info.path = path;
  info.name = path.stem().string();

  std::error_code ec;
  info.size = static_cast<uint64_t>(std::filesystem::file_size(path, ec));
  if (ec)
    info.size = 0;

  return info;
}

// ---------------------------------------------------------------------------
// Module - platform-specific handle management
// ---------------------------------------------------------------------------
Module::Module(const ModuleInfo &info) : info_(info) {
  handle_ = engine::load_shared_library(info.path);

  if (!handle_) {
    Logger::instance().error("Failed to load module: " + info.path.string());
  }
}

Module::~Module() {
  engine::unload_shared_library(handle_);
}

Module::Module(Module &&other) noexcept
    : info_(std::move(other.info_)), handle_(other.handle_) {
  other.handle_ = nullptr;
}

Module &Module::operator=(Module &&other) noexcept {
  if (this != &other) {
    engine::unload_shared_library(handle_);
    info_         = std::move(other.info_);
    handle_       = other.handle_;
    other.handle_ = nullptr;
  }
  return *this;
}

const ModuleInfo &Module::info() const {
  return info_;
}

bool Module::is_loaded() const {
  return handle_ != nullptr;
}

void *Module::symbol(const char *name) const {
  return engine::shared_library_symbol(handle_, name);
}

}  // namespace engine
