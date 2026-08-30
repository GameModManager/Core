#include "engine/core/log/logger.h"
#include "engine/deploy/vfs.h"

#include <filesystem>
#include <fstream>

namespace Deploy {

Vfs::Vfs() = default;

Vfs::~Vfs() {
  if (mounted_) {
    unmount();
  }
}

bool Vfs::deploy(const std::filesystem::path &source,
                 const std::filesystem::path &target) {
  std::lock_guard lock(mutex_);

  if (!std::filesystem::exists(source)) {
    engine::Logger::instance().error("VFS deploy: source does not exist: " +
                                     source.string());
    return false;
  }

  // Store the mapping
  file_map_[target.string()] = source.string();
  return true;
}

bool Vfs::remove(const std::filesystem::path &target) {
  std::lock_guard lock(mutex_);
  file_map_.erase(target.string());
  return true;
}

bool Vfs::mount(const std::filesystem::path &mount_point) {
  std::lock_guard lock(mutex_);

  if (mounted_) {
    engine::Logger::instance().warn("VFS already mounted at " +
                                    mount_point.string());
    return false;
  }

  // Check if fuse3 is available
  if (!is_available()) {
    engine::Logger::instance().error("FUSE not available on this system");
    return false;
  }

  mount_point_ = mount_point;
  mounted_ = true;

  engine::Logger::instance().debug("VFS mounted at " + mount_point.string() +
                                   " with " + std::to_string(file_map_.size()) +
                                   " files");

  // TODO: Actually call fuse_main() with a custom filesystem implementation
  // For now, this is a placeholder that tracks the mapping

  return true;
}

bool Vfs::unmount() {
  std::lock_guard lock(mutex_);

  if (!mounted_) {
    return false;
  }

  // TODO: Call fuse_unmount()
  mounted_ = false;
  file_map_.clear();

  engine::Logger::instance().debug("VFS unmounted from " +
                                   mount_point_.string());
  return true;
}

void Vfs::add_file(const std::string &virtual_path,
                   const std::string &source_path) {
  std::lock_guard lock(mutex_);
  file_map_[virtual_path] = source_path;
}

void Vfs::remove_file(const std::string &virtual_path) {
  std::lock_guard lock(mutex_);
  file_map_.erase(virtual_path);
}

bool Vfs::is_available() {
  // Check if fuse3 or fuse2 is installed
  // Look for /usr/lib/libfuse3.so or /usr/lib/libfuse.so
  namespace fs = std::filesystem;

  const char *fuse3_paths[] = {
      "/usr/lib/libfuse3.so",
      "/usr/lib/x86_64-linux-gnu/libfuse3.so",
      "/usr/lib64/libfuse3.so",
      "/usr/local/lib/libfuse3.so",
  };

  const char *fuse2_paths[] = {
      "/usr/lib/libfuse.so",
      "/usr/lib/x86_64-linux-gnu/libfuse.so",
      "/usr/lib64/libfuse.so",
      "/usr/local/lib/libfuse.so",
  };

  for (const auto &path : fuse3_paths) {
    if (fs::exists(path))
      return true;
  }
  for (const auto &path : fuse2_paths) {
    if (fs::exists(path))
      return true;
  }

  return false;
}

} // namespace Deploy
