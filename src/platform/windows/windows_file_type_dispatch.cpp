#ifndef GMM_PLATFORM_WINDOWS
#error "This file should only be compiled on Windows"
#endif

#include "platform/windows/windows_file_type_dispatch.h"

#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

namespace engine {

bool WindowsFileTypeDispatcher::can_launch(FileType type) const {
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

LaunchResult WindowsFileTypeDispatcher::launch(const std::filesystem::path &file,
                                               const LaunchOptions &options) const {
  if (!std::filesystem::exists(file))
    return {};

  auto ft = classify(file);
  if (ft == FileType::SharedLibrary)
    return {};

  // ShellExecuteW handles .exe, .bat, .cmd, .jar (if Java registered),
  // and any registered file type. It's the Windows equivalent of
  // xdg-open: the OS picks the right handler.
  auto file_str = file.wstring();
  auto dir_str  = file.parent_path().wstring();

  HINSTANCE hInst =
      ShellExecuteW(nullptr, L"open", file_str.c_str(), nullptr, dir_str.c_str(),
                    options.background ? SW_SHOWNOACTIVATE : SW_SHOWNORMAL);

  LaunchResult result;
  result.ok = reinterpret_cast<intptr_t>(hInst) > 32;
  return result;
}

std::unique_ptr<FileTypeDispatcher> create_file_type_dispatcher() {
  return std::make_unique<WindowsFileTypeDispatcher>();
}

}  // namespace engine
