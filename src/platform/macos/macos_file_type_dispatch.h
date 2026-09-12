#pragma once

// macOS file type dispatcher stub - fork+exec / open based launch.
// TODO: Implement full dispatch for .app, .jar, .command on macOS.

#include "engine/platform/file_type_dispatch.h"

namespace engine
{

class MacOSFileTypeDispatcher : public FileTypeDispatcher
{
public:
  [[nodiscard]] bool can_launch(FileType type) const override;

  [[nodiscard]] LaunchResult launch(const std::filesystem::path& file,
                                    const LaunchOptions& options = {}) const override;
};

}  // namespace engine
