#ifndef GMM_PLATFORM_MACOS
#error "This file should only be compiled on macOS"
#endif

#include "platform/macos/macos_file_type_dispatch.h"

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace engine {

bool MacOSFileTypeDispatcher::can_launch(FileType type) const {
  switch (type) {
  case FileType::NativeExecutable:
  case FileType::Script:
  case FileType::JavaArchive:
  case FileType::AppImage:
  case FileType::Unknown:
    return true;
  case FileType::SharedLibrary:
    return false;
  }
  return false;
}

LaunchResult MacOSFileTypeDispatcher::launch(const std::filesystem::path &file,
                                             const LaunchOptions &options) const {
  if (!std::filesystem::exists(file))
    return {};

  auto ft = classify(file);
  if (ft == FileType::SharedLibrary)
    return {};

  // "open" is the macOS equivalent of xdg-open: delegates to LaunchServices.
  const pid_t pid = fork();
  if (pid < 0)
    return {};

  if (pid == 0) {
    if (options.background)
      setsid();

    std::vector<char *> argv;
    argv.push_back(const_cast<char *>("open"));
    argv.push_back(const_cast<char *>(file.c_str()));
    argv.push_back(nullptr);

    execvp("open", argv.data());
    _exit(127);
  }

  if (options.background) {
    LaunchResult result;
    result.ok  = true;
    result.pid = static_cast<int64_t>(pid);
    return result;
  }

  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  LaunchResult result;
  result.ok  = WIFEXITED(status) && WEXITSTATUS(status) == 0;
  result.pid = static_cast<int64_t>(pid);
  return result;
}

std::unique_ptr<FileTypeDispatcher> create_file_type_dispatcher() {
  return std::make_unique<MacOSFileTypeDispatcher>();
}

}  // namespace engine
