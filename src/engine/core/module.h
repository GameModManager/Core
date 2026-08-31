#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace engine {

// ---------------------------------------------------------------------------
// ModuleInfo - metadata about a loadable module (shared library / plugin).
// ---------------------------------------------------------------------------
struct ModuleInfo {
  std::filesystem::path path;
  std::string name;
  std::string version;
  uint64_t size = 0;

  // Load module info from a file on disk.
  // Reads the file's metadata (name from filename, size from filesystem).
  static ModuleInfo from_path(const std::filesystem::path &path);
};

// ---------------------------------------------------------------------------
// Module - RAII wrapper around a platform-specific shared library handle.
// ---------------------------------------------------------------------------
class Module {
public:
  explicit Module(const ModuleInfo &info);
  ~Module();

  // Non-copyable
  Module(const Module &) = delete;
  Module &operator=(const Module &) = delete;

  // Movable
  Module(Module &&other) noexcept;
  Module &operator=(Module &&other) noexcept;

  [[nodiscard]] const ModuleInfo &info() const;
  [[nodiscard]] bool is_loaded() const;

  // Look up a symbol by name. Returns nullptr on failure.
  [[nodiscard]] void *symbol(const char *name) const;

private:
  ModuleInfo info_;
  void *handle_ = nullptr; // HMODULE on Windows, void* on POSIX
};

} // namespace engine
