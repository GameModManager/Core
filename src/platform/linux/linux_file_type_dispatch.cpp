#ifndef __linux__
#error "This file should only be compiled on Linux"
#endif

#include "platform/linux/linux_file_type_dispatch.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace engine {

// ---------------------------------------------------------------------------
// can_launch
// ---------------------------------------------------------------------------

bool LinuxFileTypeDispatcher::can_launch(FileType type) const {
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

// ---------------------------------------------------------------------------
// Launch
// ---------------------------------------------------------------------------

bool LinuxFileTypeDispatcher::ensure_executable(const std::filesystem::path &file) {
  std::error_code ec;
  auto st = std::filesystem::status(file, ec);
  if (ec)
    return false;

  auto perms = st.permissions();
  if ((perms & std::filesystem::perms::owner_exec) != std::filesystem::perms::none) {
    return true;  // already executable
  }

  std::filesystem::permissions(file,
                               perms | std::filesystem::perms::owner_exec |
                                   std::filesystem::perms::group_exec |
                                   std::filesystem::perms::others_exec,
                               ec);
  return !ec;
}

LaunchResult LinuxFileTypeDispatcher::fork_exec(
    const std::filesystem::path &executable, const std::vector<std::string> &argv,
    const std::filesystem::path &working_dir, bool background) {
  LaunchResult result;

  const pid_t pid = fork();
  if (pid < 0)
    return result;

  if (pid == 0) {
    // Child process.
    if (background)
      setsid();

    if (!working_dir.empty()) {
      std::error_code ec;
      auto canonical = std::filesystem::canonical(working_dir, ec);
      if (!ec)
        chdir(canonical.c_str());
    }

    // Redirect stdin from /dev/null so the child doesn't inherit our TTY.
    const int devnull = open("/dev/null", O_RDONLY);
    if (devnull >= 0) {
      dup2(devnull, STDIN_FILENO);
      close(devnull);
    }

    // Build the raw argv array.
    std::vector<char *> raw_argv;
    raw_argv.reserve(argv.size() + 1);
    for (const auto &arg : argv)
      raw_argv.push_back(const_cast<char *>(arg.c_str()));
    raw_argv.push_back(nullptr);

    execvp(raw_argv[0], raw_argv.data());

    // Fallback: try running through /bin/sh for scripts without a
    // proper shebang.
    std::vector<char *> sh_argv;
    sh_argv.reserve(argv.size() + 2);
    sh_argv.push_back(const_cast<char *>("sh"));
    for (const auto &arg : argv)
      sh_argv.push_back(const_cast<char *>(arg.c_str()));
    sh_argv.push_back(nullptr);

    execv("/bin/sh", sh_argv.data());
    _exit(127);
  }

  // Parent: if background, don't wait - child is fully detached.
  if (background) {
    result.ok  = true;
    result.pid = static_cast<int64_t>(pid);
    return result;
  }

  // Foreground: wait for the child.
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  result.ok  = WIFEXITED(status) && WEXITSTATUS(status) == 0;
  result.pid = static_cast<int64_t>(pid);
  return result;
}

LaunchResult LinuxFileTypeDispatcher::launch(const std::filesystem::path &file,
                                             const LaunchOptions &options) const {
  if (!std::filesystem::exists(file))
    return {};

  auto ft = classify(file);
  if (ft == FileType::SharedLibrary)
    return {};

  switch (ft) {
  case FileType::NativeExecutable: {
    ensure_executable(file);
    std::vector<std::string> argv;
    argv.push_back(file.string());
    for (const auto &a : options.args)
      argv.push_back(a);
    return fork_exec(file, argv, options.working_dir, options.background);
  }

  case FileType::Script: {
    // Ensure +x so the kernel can exec it (or sh -c fallback).
    ensure_executable(file);
    std::vector<std::string> argv;
    argv.push_back(file.string());
    for (const auto &a : options.args)
      argv.push_back(a);
    return fork_exec(file, argv, options.working_dir, options.background);
  }

  case FileType::JavaArchive: {
    std::vector<std::string> argv = {"java", "-jar", file.string()};
    for (const auto &a : options.args)
      argv.push_back(a);
    // java needs the full path to resolve classpath relative to jar.
    return fork_exec("java", argv, file.parent_path(), options.background);
  }

  case FileType::AppImage: {
    ensure_executable(file);
    std::vector<std::string> argv;
    argv.push_back(file.string());
    for (const auto &a : options.args)
      argv.push_back(a);
    return fork_exec(file, argv, options.working_dir, options.background);
  }

  case FileType::Unknown: {
    // xdg-open delegates to the system's default handler.
    std::vector<std::string> argv = {"xdg-open", file.string()};
    return fork_exec("xdg-open", argv, {}, true);
  }

  case FileType::SharedLibrary:
    return {};
  }

  return {};
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

std::unique_ptr<FileTypeDispatcher> create_file_type_dispatcher() {
  return std::make_unique<LinuxFileTypeDispatcher>();
}

}  // namespace engine
