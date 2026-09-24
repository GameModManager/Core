#pragma once

// Windows file type dispatcher stub - ShellExecute / CreateProcess based launch.
// TODO: Implement full dispatch for .exe, .bat, .jar, .ps1 on Windows.

#include "engine/platform/file_type_dispatch.h"

namespace engine {

class WindowsFileTypeDispatcher : public FileTypeDispatcher {
public:
  [[nodiscard]] bool can_launch(FileType type) const override;

  [[nodiscard]] LaunchResult launch(const std::filesystem::path &file,
                                    const LaunchOptions &options = {}) const override;
};

}  // namespace engine
