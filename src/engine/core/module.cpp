#include "engine/core/module.h"

#include "engine/core/log/logger.h"

#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace engine {

// ---------------------------------------------------------------------------
// ModuleInfo::from_path
// ---------------------------------------------------------------------------
ModuleInfo ModuleInfo::from_path(const std::filesystem::path &path) {
  ModuleInfo info;
  info.path = path;
  info.name = path.stem().string();

  std::error_code ec;
  info.size = static_cast<uint64_t>(
      std::filesystem::file_size(path, ec));
  if (ec)
    info.size = 0;

  return info;
}

// ---------------------------------------------------------------------------
// Module - platform-specific handle management
// ---------------------------------------------------------------------------
Module::Module(const ModuleInfo &info) : info_(info) {
#ifdef _WIN32
  handle_ = static_cast<void *>(
      LoadLibraryW(info.path.wstring().c_str()));
#else
  handle_ = dlopen(info.path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif

  if (!handle_) {
    Logger::instance().error("Failed to load module: " +
                             info.path.string());
  }
}

Module::~Module() {
  if (handle_) {
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(handle_));
#else
    dlclose(handle_);
#endif
  }
}

Module::Module(Module &&other) noexcept
    : info_(std::move(other.info_)), handle_(other.handle_) {
  other.handle_ = nullptr;
}

Module &Module::operator=(Module &&other) noexcept {
  if (this != &other) {
    if (handle_) {
#ifdef _WIN32
      FreeLibrary(static_cast<HMODULE>(handle_));
#else
      dlclose(handle_);
#endif
    }
    info_ = std::move(other.info_);
    handle_ = other.handle_;
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
  if (!handle_)
    return nullptr;

#ifdef _WIN32
  return reinterpret_cast<void *>(
      GetProcAddress(static_cast<HMODULE>(handle_), name));
#else
  return dlsym(handle_, name);
#endif
}

} // namespace engine
